/*
 * WT32-ETH01 PLC bridge - Mode A: Wi-Fi station with the PLC's MAC address cloned.
 *
 * The station associates with the existing Wi-Fi network using the PLC's own MAC, so every
 * frame passes through unmodified in both directions: no ARP table, no payload rewriting.
 * See Phases 3 and 4 of the implementation plan.
 *
 *   Ethernet -> Wi-Fi: EMAC RX callback filters and queues; wifi_tx_task sends with NO_MEM back-off.
 *   Wi-Fi -> Ethernet: Wi-Fi RX callback filters and transmits directly.
 *   Control (MAC learning, Wi-Fi start/stop, reconnects, setup AP) runs in the default event loop task.
 *
 * Management goes through a separate setup access point with its own MAC and lwIP netif
 * (bridge_web.c) and the serial console (bridge_console.c). The station deliberately has no
 * netif: lwIP must never see bridged frames.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_eth_driver.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ethernet_init.h"
#include "bridge.h"
#include "bridge_config.h"
#include "bridge_frames.h"

static const char *TAG = "bridge";

#define NVS_NAMESPACE               "bridge"
#define NVS_KEY_PLC_MAC             "plc_mac"
#define NVS_KEY_CHANNEL             "channel"
#define PROBE_INTERVAL_US           (2 * 1000 * 1000)
#define HOUSEKEEPING_INTERVAL_US    (10 * 1000 * 1000)
#define RESTART_DELAY_US            (1000 * 1000)
#define TX_ENQUEUE_TIMEOUT_MS       10
#define TX_MAX_BACKOFF_MS           32      // 1+2+...+32 = 63 ms total before a frame is dropped
#define TX_TASK_STACK               3072
#define TX_TASK_PRIO                6
#define FULL_SCAN_AFTER_FAILURES    3
#define SETUP_AP_RETRY_MS           30000   // slower station retries keep the setup AP usable
#define SETUP_AP_MAX_CLIENTS        2
#define MINUTES_US(m)               ((int64_t)(m) * 60 * 1000000)

#if CONFIG_BRIDGE_SETUP_AP_AT_BOOT
#define SETUP_AP_AT_BOOT            true
#else
#define SETUP_AP_AT_BOOT            false
#endif

ESP_EVENT_DEFINE_BASE(BRIDGE_EVENT);
enum {
    BRIDGE_EVENT_PLC_LEARNED,           // data: plc_learned_t
    BRIDGE_EVENT_SETUP_AP,              // data: bool (open / close)
};

typedef struct {
    uint8_t mac[BR_MAC_LEN];
    uint32_t ip;
} plc_learned_t;

typedef struct {
    uint8_t *buf;
    uint32_t len;
} tx_msg_t;

// Each counter is written by exactly one task.
static struct {
    uint32_t eth2wifi_fwd;
    uint32_t eth2wifi_queue_full;
    uint32_t eth2wifi_tx_fail;
    uint32_t wifi2eth_fwd;
    uint32_t wifi2eth_tx_fail;
    uint32_t reconnects;
} s_stats;

static bridge_config_t s_cfg;               // settings in effect since boot

static esp_eth_handle_t s_eth;
static uint8_t s_eth_mac[BR_MAC_LEN];       // the ESP32's own Ethernet MAC (used for discovery probes)
// MAC cloned onto the station. Written only in the event task while the RX paths may read it;
// a torn read during a PLC swap can at worst drop a frame.
static uint8_t s_plc_mac[BR_MAC_LEN];
static uint32_t s_plc_ip;                   // network byte order, 0 = not known yet
static volatile bool s_plc_mac_set;         // station currently carries a PLC MAC
static volatile bool s_plc_seen;            // PLC reported since the last Ethernet link-up
static volatile bool s_eth_link_up;
static volatile bool s_sta_connected;
static int s_probes_unanswered;             // since the last Ethernet link-up

// Wi-Fi state; changed only in the event task (and in app_main before Ethernet starts).
static bool s_wifi_started;
static volatile bool s_station_enabled;     // PLC MAC known and a network configured
static volatile bool s_ap_enabled;          // setup access point open
static bool s_setup_ap_ready;               // setup AP configured (valid password)
static uint8_t s_last_channel;
static uint8_t s_cached_channel;            // channel stored in NVS
static uint8_t s_bssid[BR_MAC_LEN];
static int s_connect_failures;
static int64_t s_disconnected_since_us;     // 0 while connected or when the station is off
static int64_t s_ap_last_activity_us;

static wifi_config_t s_sta_cfg;
static QueueHandle_t s_tx_queue;
static esp_timer_handle_t s_probe_timer;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_housekeeping_timer;
static esp_timer_handle_t s_restart_timer;

/* ---------- NVS cache: PLC MAC and Wi-Fi channel ---------- */

static bool cache_get(const char *key, void *data, size_t size)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = size;
    esp_err_t err = nvs_get_blob(nvs, key, data, &len);
    nvs_close(nvs);
    return err == ESP_OK && len == size;
}

static void cache_set(const char *key, const void *data, size_t size)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, key, data, size);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not cache %s in NVS: %s", key, esp_err_to_name(err));
    }
}

/* ---------- Ethernet -> Wi-Fi ---------- */

static bool egress_allowed(const uint8_t *frame, uint32_t len)
{
    if (len < ETH_HDR_LEN || !s_plc_mac_set) {
        return false;
    }
    const uint8_t *dst = frame;
    const uint8_t *src = frame + BR_MAC_LEN;
    if (!mac_equal(src, s_plc_mac)          // only the cloned PLC may use the station
            || mac_equal(dst, s_eth_mac)    // replies to our own discovery probes
            || mac_is_link_local(dst)) {    // LLDP etc. (PROFINET ports send these from port MACs anyway)
        return false;
    }
    switch (frame_ethertype(frame, len, NULL)) {
    case ETHTYPE_IPV4:
    case ETHTYPE_ARP:
        return true;
#if CONFIG_BRIDGE_FORWARD_DCP
    case ETHTYPE_PROFINET:
        return frame_is_profinet_dcp(frame, len);
#endif
    default:
        return false;
    }
}

// Runs in the EMAC RX task for every frame from the Ethernet side. Reports the PLC's
// identity to the event task once per Ethernet link-up, or whenever its MAC changes.
static void learn_from_ethernet(const uint8_t *frame, uint32_t len)
{
    static uint8_t s_reported_mac[BR_MAC_LEN];      // touched only by the EMAC RX task
    static uint32_t s_ignored_ip;                   // last mismatching sender we warned about

    const uint8_t *src = frame + BR_MAC_LEN;
    uint32_t sender_ip;
    // Only IP/ARP frames from a unicast MAC: the PLC's interface MAC. Sender IP 0.0.0.0 is an
    // ARP probe from IP-conflict detection and says nothing about the PLC's address.
    if (len < ETH_HDR_LEN || mac_is_group(src) || mac_equal(src, s_eth_mac) ||
            !frame_sender_ip(frame, len, &sender_ip) || sender_ip == 0) {
        return;
    }
    if (s_plc_ip != 0 && sender_ip != s_plc_ip) {
        // Most often the PLC itself with a different IP than configured: say so once per address.
        if (sender_ip != s_ignored_ip) {
            s_ignored_ip = sender_ip;
            ESP_LOGW(TAG, "Ignoring traffic from " IPV4_FMT " (" MACSTR "): PLC IP is set to " IPV4_FMT
                     ". If this is the PLC, change the PLC IP (setup page or console 'plcip').",
                     IPV4_ARGS(sender_ip), MAC2STR(src), IPV4_ARGS(s_plc_ip));
        }
        return;
    }
    if (s_plc_seen && mac_equal(src, s_reported_mac)) {
        return;
    }
    plc_learned_t info = { .ip = sender_ip };
    memcpy(info.mac, src, BR_MAC_LEN);
    if (esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_PLC_LEARNED, &info, sizeof(info), 0) == ESP_OK) {
        memcpy(s_reported_mac, src, BR_MAC_LEN);
        s_plc_seen = true;
    }
}

// EMAC RX callback; we own `buf` and must free it on every path that doesn't queue it.
static esp_err_t eth_to_wifi(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv)
{
    learn_from_ethernet(buf, len);
    if (s_sta_connected && egress_allowed(buf, len)) {
        tx_msg_t msg = { .buf = buf, .len = len };
        if (xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(TX_ENQUEUE_TIMEOUT_MS)) == pdTRUE) {
            return ESP_OK;                  // wifi_tx_task frees it after sending
        }
        s_stats.eth2wifi_queue_full++;
    }
    free(buf);
    return ESP_OK;
}

// Drains the queue. ESP_ERR_NO_MEM means the Wi-Fi TX buffers are full: back off briefly,
// then drop the frame and let TCP retransmit.
static void wifi_tx_task(void *arg)
{
    tx_msg_t msg;
    for (;;) {
        xQueueReceive(s_tx_queue, &msg, portMAX_DELAY);
        int err = esp_wifi_internal_tx(WIFI_IF_STA, msg.buf, (uint16_t)msg.len);
        for (int delay_ms = 1; err == ESP_ERR_NO_MEM && delay_ms <= TX_MAX_BACKOFF_MS; delay_ms *= 2) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            err = esp_wifi_internal_tx(WIFI_IF_STA, msg.buf, (uint16_t)msg.len);
        }
        if (err == ESP_OK) {
            s_stats.eth2wifi_fwd++;
        } else {
            s_stats.eth2wifi_tx_fail++;
        }
        free(msg.buf);
    }
}

/* ---------- Wi-Fi -> Ethernet ---------- */

static bool ingress_allowed(const uint8_t *frame, uint16_t len)
{
    if (len < ETH_HDR_LEN) {
        return false;
    }
    const uint8_t *dst = frame;
    const uint8_t *src = frame + BR_MAC_LEN;
    if (!mac_is_group(dst) && !mac_equal(dst, s_plc_mac)) {
        return false;
    }
    if (mac_equal(src, s_plc_mac)) {        // our own broadcasts reflected by the AP
        return false;
    }
#if CONFIG_BRIDGE_DROP_IPV6
    if (frame_ethertype(frame, len, NULL) == ETHTYPE_IPV6) {
        return false;
    }
#endif
    return true;
}

// Wi-Fi RX callback (Wi-Fi task): never block, always release `eb`.
static esp_err_t wifi_to_eth(void *buffer, uint16_t len, void *eb)
{
    if (s_eth_link_up && ingress_allowed(buffer, len)) {
        if (esp_eth_transmit(s_eth, buffer, len) == ESP_OK) {
            s_stats.wifi2eth_fwd++;
        } else {
            s_stats.wifi2eth_tx_fail++;
        }
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

/* ---------- Wi-Fi control (event task) ---------- */

// Tells the network the PLC's IP now lives behind this station, refreshing ARP caches.
static void send_gratuitous_arp(void)
{
    if (s_plc_ip == 0 || !s_sta_connected) {
        return;
    }
    uint8_t frame[ETH_MIN_FRAME_LEN];
    size_t len = build_arp_request(frame, s_plc_mac, s_plc_ip, s_plc_ip);
    int err = esp_wifi_internal_tx(WIFI_IF_STA, frame, (uint16_t)len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Gratuitous ARP sent for " IPV4_FMT, IPV4_ARGS(s_plc_ip));
    } else {
        ESP_LOGW(TAG, "Gratuitous ARP failed: %s", esp_err_to_name(err));
    }
}

// Arms a single (re)connect attempt; a request while one is pending is ignored.
static void schedule_connect(uint32_t delay_ms)
{
    if (!esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_start_once(s_reconnect_timer, (uint64_t)(delay_ms ? delay_ms : 10) * 1000);
    }
}

static void reconnect_timer_cb(void *arg)
{
    if (s_wifi_started && s_station_enabled && !s_sta_connected) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }
    }
}

static wifi_mode_t wanted_wifi_mode(void)
{
    if (s_station_enabled) {
        return s_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    }
    return s_ap_enabled ? WIFI_MODE_AP : WIFI_MODE_NULL;
}

// (Re)starts the Wi-Fi driver in the wanted mode. Needed whenever the station MAC changes,
// because the MAC can only be set while the station interface is disabled.
static void wifi_restart(void)
{
    if (s_wifi_started) {
        s_wifi_started = false;
        s_sta_connected = false;
        ESP_ERROR_CHECK(esp_wifi_stop());
    }
    wifi_mode_t mode = wanted_wifi_mode();
    if (mode == WIFI_MODE_NULL) {
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    if (s_station_enabled) {
        ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, s_plc_mac));
        s_connect_failures = 0;
        s_disconnected_since_us = esp_timer_get_time();
    }
    s_wifi_started = true;
    ESP_ERROR_CHECK(esp_wifi_start());
    if (s_station_enabled) {
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));     // modem sleep adds 100-300 ms latency spikes
        ESP_LOGI(TAG, "Wi-Fi station started as " MACSTR ", joining \"%s\"", MAC2STR(s_plc_mac), s_cfg.sta_ssid);
    }
}

static void setup_ap_set(bool on)
{
    if (on && !s_setup_ap_ready) {
        ESP_LOGE(TAG, "Setup access point unavailable: set a password of 8-63 characters (console 'setuppass')");
        return;
    }
    s_ap_last_activity_us = esp_timer_get_time();
    if (on == s_ap_enabled) {
        return;
    }
    s_ap_enabled = on;
    if (!s_wifi_started) {
        wifi_restart();
    } else if (wanted_wifi_mode() == WIFI_MODE_NULL) {
        s_wifi_started = false;
        ESP_ERROR_CHECK(esp_wifi_stop());
    } else {
        // Switching between STA and AP+STA at runtime keeps the station associated.
        ESP_ERROR_CHECK(esp_wifi_set_mode(wanted_wifi_mode()));
    }
    if (on) {
        ESP_LOGI(TAG, "Setup access point \"%s\" open: http://192.168.4.1", CONFIG_BRIDGE_SETUP_AP_SSID);
    } else {
        ESP_LOGI(TAG, "Setup access point closed");
    }
}

// (Re)starts the station with `mac` as its address. Must run in the event task.
static void apply_station_mac(const uint8_t mac[BR_MAC_LEN])
{
    memcpy(s_plc_mac, mac, BR_MAC_LEN);
    s_plc_mac_set = true;
    s_station_enabled = s_cfg.sta_ssid[0] != '\0';
    if (!s_station_enabled) {
        ESP_LOGE(TAG, "No Wi-Fi network configured: set it on the setup page or with the console command 'wifi'");
    }
    wifi_restart();
}

static void on_plc_learned(const plc_learned_t *info)
{
    esp_timer_stop(s_probe_timer);
    bool ip_learned = s_plc_ip != info->ip;
    s_plc_ip = info->ip;

    if (s_plc_mac_set && mac_equal(s_plc_mac, info->mac)) {
        ESP_LOGI(TAG, "PLC confirmed: " MACSTR " at " IPV4_FMT, MAC2STR(info->mac), IPV4_ARGS(s_plc_ip));
        if (ip_learned) {
            send_gratuitous_arp();
        }
        return;
    }
    ESP_LOGI(TAG, "PLC found: " MACSTR " at " IPV4_FMT " - cloning its MAC onto the Wi-Fi station",
             MAC2STR(info->mac), IPV4_ARGS(s_plc_ip));
    cache_set(NVS_KEY_PLC_MAC, info->mac, BR_MAC_LEN);
    apply_station_mac(info->mac);
}

static void bridge_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case BRIDGE_EVENT_PLC_LEARNED:
        on_plc_learned((const plc_learned_t *)data);
        break;
    case BRIDGE_EVENT_SETUP_AP:
        setup_ap_set(*(const bool *)data);
        break;
    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        schedule_connect(0);
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *ev = data;
        s_last_channel = ev->channel;
        memcpy(s_bssid, ev->bssid, BR_MAC_LEN);
        s_connect_failures = 0;
        s_disconnected_since_us = 0;
        ESP_ERROR_CHECK(esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_to_eth));
        s_sta_connected = true;
        ESP_LOGI(TAG, "Wi-Fi connected to " MACSTR " on channel %u", MAC2STR(ev->bssid), ev->channel);
        send_gratuitous_arp();
        if (ev->channel != s_cached_channel) {
            s_cached_channel = ev->channel;
            cache_set(NVS_KEY_CHANNEL, &s_cached_channel, sizeof(s_cached_channel));
        }
        bridge_mark_app_valid();
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        bool was_connected = s_sta_connected;
        s_sta_connected = false;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        if (s_disconnected_since_us == 0) {
            s_disconnected_since_us = esp_timer_get_time();
        }
        if (was_connected) {
            s_stats.reconnects++;
        }
        if (!s_wifi_started || !s_station_enabled) {
            break;
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %u, rssi %d)", ev->reason, ev->rssi);

        // Retry immediately, then 1 s, 2 s, 5 s. The first attempts start on the last known
        // channel; after that fall back to a full scan in case the AP changed channel. While the
        // setup AP is open, retry only every 30 s: each scan takes the radio off the AP's channel.
        static const uint32_t backoff_ms[] = { 0, 1000, 2000, 5000 };
        int step = s_connect_failures < 3 ? s_connect_failures : 3;
        s_connect_failures++;
        bool fast = s_last_channel != 0 && s_connect_failures <= FULL_SCAN_AFTER_FAILURES;
        s_sta_cfg.sta.scan_method = fast ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
        s_sta_cfg.sta.channel = fast ? s_last_channel : 0;
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
        bool slow = s_ap_enabled && s_connect_failures > FULL_SCAN_AFTER_FAILURES;
        schedule_connect(slow ? SETUP_AP_RETRY_MS : backoff_ms[step]);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *ev = data;
        s_ap_last_activity_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Setup access point: " MACSTR " joined", MAC2STR(ev->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *ev = data;
        s_ap_last_activity_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Setup access point: " MACSTR " left", MAC2STR(ev->mac));
        break;
    }
    default:
        break;
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_ap();         // setup page only; its handlers ignore the netif-less station
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // Both interfaces must be enabled to store their configurations; wifi_restart() sets the real mode.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    strlcpy((char *)s_sta_cfg.sta.ssid, s_cfg.sta_ssid, sizeof(s_sta_cfg.sta.ssid));
    strlcpy((char *)s_sta_cfg.sta.password, s_cfg.sta_password, sizeof(s_sta_cfg.sta.password));
    s_last_channel = s_cached_channel;
    s_sta_cfg.sta.scan_method = s_cached_channel ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    s_sta_cfg.sta.channel = s_cached_channel;
    s_sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    s_sta_cfg.sta.threshold.authmode = s_cfg.sta_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg));

    s_setup_ap_ready = bridge_config_check_setup_password(s_cfg.setup_password) == NULL;
    if (s_setup_ap_ready) {
        wifi_config_t ap_cfg = {
            .ap = {
                .channel = s_cached_channel ? s_cached_channel : 1,     // follows the station once connected
                .authmode = WIFI_AUTH_WPA2_PSK,
                .max_connection = SETUP_AP_MAX_CLIENTS,
            },
        };
        strlcpy((char *)ap_cfg.ap.ssid, CONFIG_BRIDGE_SETUP_AP_SSID, sizeof(ap_cfg.ap.ssid));
        ap_cfg.ap.ssid_len = strlen((const char *)ap_cfg.ap.ssid);
        strlcpy((char *)ap_cfg.ap.password, s_cfg.setup_password, sizeof(ap_cfg.ap.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    } else {
        ESP_LOGW(TAG, "Setup access point disabled: its password must be 8-63 characters (console 'setuppass')");
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
}

/* ---------- Ethernet ---------- */

// Asks "who has <PLC IP>?" with sender IP 0.0.0.0, so the PLC's ARP cache isn't touched.
static void probe_timer_cb(void *arg)
{
    if (!s_eth_link_up || s_plc_seen || s_plc_ip == 0) {
        esp_timer_stop(s_probe_timer);
        return;
    }
    // Warn after 10 s without an answer, then once a minute.
    if (++s_probes_unanswered % 30 == 5) {
        ESP_LOGW(TAG, "No answer from " IPV4_FMT " on Ethernet. Is that the PLC's IP?", IPV4_ARGS(s_plc_ip));
    }
    uint8_t frame[ETH_MIN_FRAME_LEN];
    size_t len = build_arp_request(frame, s_eth_mac, 0, s_plc_ip);
    esp_eth_transmit(s_eth, frame, len);
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        s_eth_link_up = true;
        s_plc_seen = false;                 // re-confirm the PLC: it may have been swapped
        s_probes_unanswered = 0;
        if (s_plc_ip != 0 && !esp_timer_is_active(s_probe_timer)) {
            esp_timer_start_periodic(s_probe_timer, PROBE_INTERVAL_US);
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_eth_link_up = false;
        esp_timer_stop(s_probe_timer);
        break;
    default:
        break;
    }
}

static void bridge_ethernet_init(void)
{
    uint8_t eth_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_cnt));
    if (eth_cnt == 0) {
        ESP_LOGE(TAG, "No Ethernet interface initialized");
        abort();
    }
    s_eth = eth_handles[0];
    free(eth_handles);

    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, s_eth_mac));
    ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth, eth_to_wifi, NULL));
    // Promiscuous: frames sent by the PLC are addressed to other MACs (gateway, PC), not to us.
    bool promiscuous = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promiscuous));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth));
}

/* ---------- Management interface (bridge.h) ---------- */

static int setup_ap_client_count(void)
{
    wifi_sta_list_t list;
    return (s_ap_enabled && esp_wifi_ap_get_sta_list(&list) == ESP_OK) ? list.num : 0;
}

void bridge_get_status(bridge_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->eth_link_up = s_eth_link_up;
    status->wifi_connected = s_sta_connected;
    if (status->wifi_connected) {
        esp_wifi_sta_get_rssi(&status->rssi);
        status->channel = s_last_channel;
        memcpy(status->bssid, s_bssid, BR_MAC_LEN);
    }
    status->plc_mac_known = s_plc_mac_set;
    status->plc_seen = s_plc_seen;
    memcpy(status->plc_mac, s_plc_mac, BR_MAC_LEN);
    status->plc_ip = s_plc_ip;
    status->eth2wifi_fwd = s_stats.eth2wifi_fwd;
    status->eth2wifi_queue_full = s_stats.eth2wifi_queue_full;
    status->eth2wifi_tx_fail = s_stats.eth2wifi_tx_fail;
    status->wifi2eth_fwd = s_stats.wifi2eth_fwd;
    status->wifi2eth_tx_fail = s_stats.wifi2eth_tx_fail;
    status->reconnects = s_stats.reconnects;
    status->setup_ap_active = s_ap_enabled;
    status->setup_ap_clients = setup_ap_client_count();
}

void bridge_request_setup_ap(bool on)
{
    esp_event_post(BRIDGE_EVENT, BRIDGE_EVENT_SETUP_AP, &on, sizeof(on), pdMS_TO_TICKS(100));
}

static void restart_timer_cb(void *arg)
{
    esp_restart();
}

void bridge_restart_soon(void)
{
    esp_timer_start_once(s_restart_timer, RESTART_DELAY_US);   // a second request is ignored
}

void bridge_mark_app_valid(void)
{
    static bool s_checked;
    if (s_checked) {
        return;
    }
    s_checked = true;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Firmware update confirmed: %s", esp_err_to_name(err));
    }
}

/* ---------- Housekeeping (esp_timer task) ---------- */

static void housekeeping_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    int64_t since = s_disconnected_since_us;
    bool wifi_down = s_wifi_started && s_station_enabled && !s_sta_connected && since != 0;
    int ap_clients = setup_ap_client_count();
    if (ap_clients > 0) {
        s_ap_last_activity_us = now;
    }

#if CONFIG_BRIDGE_SETUP_AP_AFTER_FAIL_MIN > 0
    if (wifi_down && !s_ap_enabled && s_setup_ap_ready &&
            now - since > MINUTES_US(CONFIG_BRIDGE_SETUP_AP_AFTER_FAIL_MIN)) {
        ESP_LOGW(TAG, "Wi-Fi down for %d min - opening the setup access point", CONFIG_BRIDGE_SETUP_AP_AFTER_FAIL_MIN);
        bridge_request_setup_ap(true);
    }
#endif
    // Close the setup AP only while the bridge itself is reachable through Wi-Fi.
    if (s_ap_enabled && s_sta_connected && ap_clients == 0 &&
            now - s_ap_last_activity_us > MINUTES_US(CONFIG_BRIDGE_SETUP_AP_IDLE_MIN)) {
        ESP_LOGI(TAG, "Setup access point idle for %d min", CONFIG_BRIDGE_SETUP_AP_IDLE_MIN);
        bridge_request_setup_ap(false);
    }
#if CONFIG_BRIDGE_WIFI_WATCHDOG_MIN > 0
    // Never restart under someone who is using the setup page.
    if (wifi_down && ap_clients == 0 && now - since > MINUTES_US(CONFIG_BRIDGE_WIFI_WATCHDOG_MIN)) {
        ESP_LOGE(TAG, "No Wi-Fi connection for %d min - restarting", CONFIG_BRIDGE_WIFI_WATCHDOG_MIN);
        esp_restart();
    }
#endif
#if CONFIG_BRIDGE_STATS_INTERVAL_S > 0
    static int64_t s_last_stats_us;
    if (now - s_last_stats_us >= (int64_t)CONFIG_BRIDGE_STATS_INTERVAL_S * 1000000) {
        s_last_stats_us = now;
        int rssi = 0;
        if (s_sta_connected) {
            esp_wifi_sta_get_rssi(&rssi);
        }
        ESP_LOGI(TAG, "eth %s, wifi %s (rssi %d) | eth->wifi %" PRIu32 " (queue full %" PRIu32 ", tx fail %" PRIu32
                 ") | wifi->eth %" PRIu32 " (tx fail %" PRIu32 ") | reconnects %" PRIu32 "%s",
                 s_eth_link_up ? "up" : "down", s_sta_connected ? "up" : "down", rssi,
                 s_stats.eth2wifi_fwd, s_stats.eth2wifi_queue_full, s_stats.eth2wifi_tx_fail,
                 s_stats.wifi2eth_fwd, s_stats.wifi2eth_tx_fail, s_stats.reconnects,
                 s_ap_enabled ? " | setup AP open" : "");
    }
#endif
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(BRIDGE_EVENT, ESP_EVENT_ANY_ID, bridge_event_handler, NULL));

    bridge_config_load(&s_cfg);
    s_plc_ip = s_cfg.plc_ip;
    if (s_plc_ip != 0) {
        ESP_LOGI(TAG, "PLC IP " IPV4_FMT, IPV4_ARGS(s_plc_ip));
    } else {
        ESP_LOGI(TAG, "PLC IP not set - learning it from the PLC's first IP/ARP frame");
    }

    s_tx_queue = xQueueCreate(CONFIG_BRIDGE_TX_QUEUE_LEN, sizeof(tx_msg_t));
    assert(s_tx_queue);
    BaseType_t task_ok = xTaskCreate(wifi_tx_task, "wifi_tx", TX_TASK_STACK, NULL, TX_TASK_PRIO, NULL);
    assert(task_ok == pdPASS);

    const esp_timer_create_args_t probe_args = { .callback = probe_timer_cb, .name = "plc_probe" };
    const esp_timer_create_args_t reconnect_args = { .callback = reconnect_timer_cb, .name = "wifi_reconnect" };
    const esp_timer_create_args_t housekeeping_args = { .callback = housekeeping_cb, .name = "housekeeping" };
    const esp_timer_create_args_t restart_args = { .callback = restart_timer_cb, .name = "restart" };
    ESP_ERROR_CHECK(esp_timer_create(&probe_args, &s_probe_timer));
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_timer_create(&housekeeping_args, &s_housekeeping_timer));
    ESP_ERROR_CHECK(esp_timer_create(&restart_args, &s_restart_timer));

    cache_get(NVS_KEY_CHANNEL, &s_cached_channel, sizeof(s_cached_channel));
    wifi_init();

    // Decide the initial Wi-Fi mode before Ethernet starts, so nothing can race with it:
    // the station starts straight away with the cached PLC MAC (re-confirmed once Ethernet is
    // up), and the setup AP opens after boot or whenever no network is configured.
    uint8_t cached_mac[BR_MAC_LEN];
    if (cache_get(NVS_KEY_PLC_MAC, cached_mac, BR_MAC_LEN)) {
        ESP_LOGI(TAG, "Using cached PLC MAC " MACSTR, MAC2STR(cached_mac));
        memcpy(s_plc_mac, cached_mac, BR_MAC_LEN);
        s_plc_mac_set = true;
        s_station_enabled = s_cfg.sta_ssid[0] != '\0';
    } else {
        ESP_LOGI(TAG, "No cached PLC MAC - waiting for the PLC on Ethernet");
    }
    if (s_cfg.sta_ssid[0] == '\0') {
        ESP_LOGW(TAG, "No Wi-Fi network configured: set it on the setup page or with the console command 'wifi'");
    }
    s_ap_enabled = s_setup_ap_ready && (SETUP_AP_AT_BOOT || s_cfg.sta_ssid[0] == '\0');
    s_ap_last_activity_us = esp_timer_get_time();
    wifi_restart();
    if (s_ap_enabled) {
        ESP_LOGI(TAG, "Setup access point \"%s\" open: http://192.168.4.1 (closes after %d idle min)",
                 CONFIG_BRIDGE_SETUP_AP_SSID, CONFIG_BRIDGE_SETUP_AP_IDLE_MIN);
    }

    bridge_web_start();
#if CONFIG_BRIDGE_CONSOLE
    bridge_console_start();
#endif
    bridge_ethernet_init();
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_housekeeping_timer, HOUSEKEEPING_INTERVAL_US));
}
