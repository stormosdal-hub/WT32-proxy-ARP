/*
 * MAC-NAT for other devices on the switch: see bridge_nat.h.
 *
 * The table is keyed by IP and written from the EMAC RX task, read from the Wi-Fi RX task and
 * the management side, so every access holds a spinlock for a few dozen instructions. Devices
 * are kept until their slot is needed for a new one (least recently seen first), and saved in
 * NVS, so a quiet device stays reachable after the bridge restarts.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "bridge_frames.h"
#include "bridge_nat.h"

static const char *TAG = "bridge_nat";

#define NVS_NAMESPACE   "bridge"                // erased by factory-reset with the other caches
#define NVS_KEY_TABLE   "nat"

#define ETH_OFF_TYPE    12
#define ARP_OFF_SHA     22                      // ARP fields, as offsets in an untagged frame
#define ARP_OFF_THA     32
#define ARP_OFF_TPA     38
#define ARP_FRAME_LEN   42
#define IPV4_OFF_DST    30
#define IPV4_MIN_FRAME  34

typedef struct {
    bool used;
    uint8_t mac[BR_MAC_LEN];
    uint32_t ip;
    int64_t last_seen_us;                       // 0 = restored from NVS, not seen since boot
    uint32_t to_wifi;
    uint32_t to_eth;
} entry_t;

typedef struct {                                // NVS record
    uint8_t mac[BR_MAC_LEN];
    uint32_t ip;
} __attribute__((packed)) saved_entry_t;

static entry_t *s_table;
static int s_max;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty;

static uint16_t ethertype(const uint8_t *frame)
{
    return (uint16_t)((frame[ETH_OFF_TYPE] << 8) | frame[ETH_OFF_TYPE + 1]);
}

// Callers hold s_lock.
static entry_t *find_by_ip(uint32_t ip)
{
    for (int i = 0; i < s_max; i++) {
        if (s_table[i].used && s_table[i].ip == ip) {
            return &s_table[i];
        }
    }
    return NULL;
}

static entry_t *find_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < s_max; i++) {
        if (s_table[i].used && mac_equal(s_table[i].mac, mac)) {
            return &s_table[i];
        }
    }
    return NULL;
}

// A free slot, else the device seen least recently.
static entry_t *slot_for_new(void)
{
    entry_t *oldest = &s_table[0];
    for (int i = 0; i < s_max; i++) {
        if (!s_table[i].used) {
            return &s_table[i];
        }
        if (s_table[i].last_seen_us < oldest->last_seen_us) {
            oldest = &s_table[i];
        }
    }
    return oldest;
}

void nat_init(int max_devices)
{
    s_max = max_devices < NAT_MAX_DEVICES ? max_devices : NAT_MAX_DEVICES;
    s_table = calloc(s_max, sizeof(entry_t));
    assert(s_table);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    saved_entry_t saved[NAT_MAX_DEVICES];
    size_t len = sizeof(saved);
    if (nvs_get_blob(nvs, NVS_KEY_TABLE, saved, &len) == ESP_OK) {
        int count = (int)(len / sizeof(saved_entry_t));
        for (int i = 0; i < count && i < s_max; i++) {
            s_table[i].used = true;
            memcpy(s_table[i].mac, saved[i].mac, BR_MAC_LEN);
            s_table[i].ip = saved[i].ip;
            ESP_LOGI(TAG, "Restored device " IPV4_FMT " (" MACSTR ")", IPV4_ARGS(saved[i].ip), MAC2STR(saved[i].mac));
        }
    }
    nvs_close(nvs);
}

void nat_save_if_changed(void)
{
    if (!s_dirty || !s_table) {
        return;
    }
    saved_entry_t saved[NAT_MAX_DEVICES];
    size_t count = 0;
    portENTER_CRITICAL(&s_lock);
    s_dirty = false;
    for (int i = 0; i < s_max; i++) {
        if (s_table[i].used) {
            memcpy(saved[count].mac, s_table[i].mac, BR_MAC_LEN);
            saved[count].ip = s_table[i].ip;
            count++;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY_TABLE, saved, count * sizeof(saved_entry_t));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the device table: %s", esp_err_to_name(err));
        s_dirty = true;                     // try again next time
    }
}

void nat_learn(const uint8_t *mac, uint32_t ip)
{
    enum { UNCHANGED, ADDED, MOVED } change = UNCHANGED;
    uint32_t replaced_ip = 0;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    entry_t *e = find_by_ip(ip);
    if (!e) {
        e = slot_for_new();
        if (e->used) {
            replaced_ip = e->ip;
        }
        *e = (entry_t){ .used = true, .ip = ip };
        memcpy(e->mac, mac, BR_MAC_LEN);
        change = ADDED;
    } else if (!mac_equal(e->mac, mac)) {
        memcpy(e->mac, mac, BR_MAC_LEN);    // the address moved to another device
        e->to_wifi = e->to_eth = 0;
        change = MOVED;
    }
    e->last_seen_us = now;
    if (change != UNCHANGED) {
        s_dirty = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (change != UNCHANGED) {
        ESP_LOGI(TAG, "%s " IPV4_FMT " (" MACSTR ")", change == ADDED ? "Bridging device" : "Device moved:",
                 IPV4_ARGS(ip), MAC2STR(mac));
    }
    if (replaced_ip) {
        ESP_LOGW(TAG, "Device table full: dropped " IPV4_FMT, IPV4_ARGS(replaced_ip));
    }
}

bool nat_egress(uint8_t *frame, uint32_t len, const uint8_t *station_mac)
{
    if (len < ETH_HDR_LEN) {
        return false;
    }
    uint8_t *dst = frame;
    uint8_t *src = frame + BR_MAC_LEN;
    uint16_t type = ethertype(frame);
    // IPv4 and ARP only; nothing for link-local groups (LLDP, ...) or for the PLC.
    if ((type != ETHTYPE_IPV4 && type != ETHTYPE_ARP) || mac_is_link_local(dst) || mac_equal(dst, station_mac)) {
        return false;
    }
    if (type == ETHTYPE_ARP && len < ARP_FRAME_LEN) {
        return false;
    }
    // Count the frame for its sender IP: one device can have several (e.g. a link-local one too).
    uint32_t sender_ip = 0;
    frame_sender_ip(frame, len, &sender_ip);

    portENTER_CRITICAL(&s_lock);
    // Unicast to another device on the switch is local traffic (e.g. flooded before the
    // switch learned the address): it must not go to Wi-Fi.
    bool local = !mac_is_group(dst) && find_by_mac(dst) != NULL;
    entry_t *e = (local || sender_ip == 0) ? NULL : find_by_ip(sender_ip);
    if (e && mac_equal(e->mac, src)) {
        e->to_wifi++;
    }
    portEXIT_CRITICAL(&s_lock);
    if (local) {
        return false;
    }

    if (type == ETHTYPE_ARP && mac_equal(frame + ARP_OFF_SHA, src)) {
        memcpy(frame + ARP_OFF_SHA, station_mac, BR_MAC_LEN);
    }
    memcpy(src, station_mac, BR_MAC_LEN);
    return true;
}

bool nat_ingress(uint8_t *frame, uint16_t len, const uint8_t *station_mac)
{
    if (len < ETH_HDR_LEN) {
        return false;
    }
    uint16_t type = ethertype(frame);
    uint32_t ip;
    if (type == ETHTYPE_IPV4 && len >= IPV4_MIN_FRAME) {
        memcpy(&ip, frame + IPV4_OFF_DST, 4);
    } else if (type == ETHTYPE_ARP && len >= ARP_FRAME_LEN) {
        memcpy(&ip, frame + ARP_OFF_TPA, 4);
    } else {
        return false;
    }

    uint8_t mac[BR_MAC_LEN];
    portENTER_CRITICAL(&s_lock);
    entry_t *e = find_by_ip(ip);
    if (e) {
        memcpy(mac, e->mac, BR_MAC_LEN);
        e->to_eth++;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!e) {
        return false;
    }

    memcpy(frame, mac, BR_MAC_LEN);
    // ARP replies (and unicast ARP requests that revalidate a cache entry) name the station as
    // the target hardware address.
    if (type == ETHTYPE_ARP && mac_equal(frame + ARP_OFF_THA, station_mac)) {
        memcpy(frame + ARP_OFF_THA, mac, BR_MAC_LEN);
    }
    return true;
}

size_t nat_get_devices(nat_device_t *out, size_t max)
{
    if (!s_table) {
        return 0;
    }
    int64_t now = esp_timer_get_time();
    size_t count = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_max && count < max; i++) {
        const entry_t *e = &s_table[i];
        if (!e->used) {
            continue;
        }
        nat_device_t *d = &out[count++];
        memcpy(d->mac, e->mac, BR_MAC_LEN);
        d->ip = e->ip;
        d->to_wifi = e->to_wifi;
        d->to_eth = e->to_eth;
        d->idle_s = e->last_seen_us ? (int32_t)((now - e->last_seen_us) / 1000000) : -1;
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}
