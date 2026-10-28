/*
 * Setup page: HTTP server reachable through the setup access point (http://192.168.4.1).
 *
 *   GET  /              page (web/index.html, embedded)
 *   GET  /api/status    links, PLC, counters, firmware (JSON)
 *   GET  /api/config    current settings without passwords (JSON)
 *   POST /api/config    save settings (form-encoded), then restart
 *   POST /api/ota       firmware image as the raw request body, then restart
 *   POST /api/restart
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "bridge.h"
#include "bridge_config.h"
#include "bridge_frames.h"

static const char *TAG = "web";

#define MAX_FORM_LEN    1024
#define OTA_CHUNK_LEN   4096
#define OTA_MAX_TIMEOUTS 5

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* ---------- small helpers ---------- */

typedef struct {
    char *buf;
    size_t size;
    size_t len;
} strbuf_t;

static void sb_printf(strbuf_t *sb, const char *fmt, ...)
{
    if (sb->len >= sb->size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->size - sb->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb->len += (size_t)n;
    }
}

// Appends a JSON string literal (with quotes).
static void sb_json_str(strbuf_t *sb, const char *s)
{
    sb_printf(sb, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            sb_printf(sb, "\\%c", c);
        } else if (c < 0x20) {
            sb_printf(sb, "\\u%04x", c);
        } else {
            sb_printf(sb, "%c", c);
        }
    }
    sb_printf(sb, "\"");
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_result(httpd_req_t *req, const char *error)
{
    char json[160];
    strbuf_t sb = { json, sizeof(json), 0 };
    if (error) {
        sb_printf(&sb, "{\"ok\":false,\"error\":");
        sb_json_str(&sb, error);
        sb_printf(&sb, "}");
        return send_json(req, "400 Bad Request", json);
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "crash (panic)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout (power dip)";
    default:                return "other";
    }
}

static void url_decode(char *s)
{
    char *out = s;
    for (const char *in = s; *in; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (in[0] == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[3] = { in[1], in[2], '\0' };
            *out++ = (char)strtol(hex, NULL, 16);
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

// 1 = found (decoded into val), 0 = missing, -1 = too long.
static int form_field(const char *body, const char *key, char *val, size_t size)
{
    char raw[200];          // room for a URL-encoded 63-character password
    esp_err_t err = httpd_query_key_value(body, key, raw, sizeof(raw));
    if (err == ESP_ERR_NOT_FOUND) {
        return 0;
    }
    if (err != ESP_OK) {
        return -1;
    }
    url_decode(raw);
    if (strlen(raw) >= size) {
        return -1;
    }
    strlcpy(val, raw, size);
    return 1;
}

/* ---------- handlers ---------- */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);   // EMBED_TXTFILES adds a NUL
}

static esp_err_t status_get(httpd_req_t *req)
{
    bridge_mark_app_valid();        // a working setup page counts as a healthy firmware update

    bridge_status_t st;
    bridge_get_status(&st);
    bridge_config_t cfg;
    bridge_config_load(&cfg);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    char json[1024];
    strbuf_t sb = { json, sizeof(json), 0 };
    sb_printf(&sb, "{\"uptime_s\":%" PRId64 ",\"reset_reason\":\"%s\",\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32,
              esp_timer_get_time() / 1000000, reset_reason_name(esp_reset_reason()),
              esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    sb_printf(&sb, ",\"firmware\":{\"version\":");
    sb_json_str(&sb, app->version);
    sb_printf(&sb, ",\"built\":\"%s %s\",\"idf\":", app->date, app->time);
    sb_json_str(&sb, app->idf_ver);
    sb_printf(&sb, ",\"partition\":\"%s\"}", running ? running->label : "?");
    sb_printf(&sb, ",\"ethernet\":{\"link\":%s}", st.eth_link_up ? "true" : "false");
    sb_printf(&sb, ",\"wifi\":{\"connected\":%s,\"ssid\":", st.wifi_connected ? "true" : "false");
    sb_json_str(&sb, cfg.sta_ssid);
    sb_printf(&sb, ",\"rssi\":%d,\"channel\":%u,\"bssid\":\"" MACSTR "\"}", st.rssi, st.channel, MAC2STR(st.bssid));
    sb_printf(&sb, ",\"plc\":{\"mac\":");
    if (st.plc_mac_known) {
        sb_printf(&sb, "\"" MACSTR "\"", MAC2STR(st.plc_mac));
    } else {
        sb_printf(&sb, "null");
    }
    sb_printf(&sb, ",\"seen\":%s,\"ip\":", st.plc_seen ? "true" : "false");
    if (st.plc_ip != 0) {
        sb_printf(&sb, "\"" IPV4_FMT "\"", IPV4_ARGS(st.plc_ip));
    } else {
        sb_printf(&sb, "null");
    }
    sb_printf(&sb, "},\"counters\":{\"eth2wifi\":%" PRIu32 ",\"eth2wifi_queue_full\":%" PRIu32
              ",\"eth2wifi_tx_fail\":%" PRIu32 ",\"wifi2eth\":%" PRIu32 ",\"wifi2eth_tx_fail\":%" PRIu32
              ",\"reconnects\":%" PRIu32 "}",
              st.eth2wifi_fwd, st.eth2wifi_queue_full, st.eth2wifi_tx_fail,
              st.wifi2eth_fwd, st.wifi2eth_tx_fail, st.reconnects);
    sb_printf(&sb, ",\"setup_ap\":{\"active\":%s,\"clients\":%d}}", st.setup_ap_active ? "true" : "false",
              st.setup_ap_clients);
    if (sb.len >= sb.size) {
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"status too large\"}");
    }
    return send_json(req, "200 OK", json);
}

static esp_err_t config_get(httpd_req_t *req)
{
    bridge_config_t cfg;
    bridge_config_load(&cfg);
    char json[256];
    strbuf_t sb = { json, sizeof(json), 0 };
    sb_printf(&sb, "{\"ssid\":");
    sb_json_str(&sb, cfg.sta_ssid);
    sb_printf(&sb, ",\"open\":%s,\"plc_ip\":", cfg.sta_password[0] ? "false" : "true");
    if (cfg.plc_ip != 0) {
        sb_printf(&sb, "\"" IPV4_FMT "\"", IPV4_ARGS(cfg.plc_ip));
    } else {
        sb_printf(&sb, "\"\"");
    }
    sb_printf(&sb, ",\"setup_ssid\":");
    sb_json_str(&sb, CONFIG_BRIDGE_SETUP_AP_SSID);
    sb_printf(&sb, "}");
    return send_json(req, "200 OK", json);
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_FORM_LEN) {
        return send_result(req, "Unexpected request size");
    }
    char body[MAX_FORM_LEN + 1];
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    body[received] = '\0';

    bridge_config_t cfg;
    bridge_config_load(&cfg);
    char value[65];
    const char *problem = NULL;

    // Network name (required)
    if (form_field(body, "ssid", value, sizeof(cfg.sta_ssid)) != 1 || (problem = bridge_config_check_ssid(value))) {
        return send_result(req, problem ? problem : "The network name must be 1-32 characters");
    }
    strlcpy(cfg.sta_ssid, value, sizeof(cfg.sta_ssid));

    // Network password: "open" clears it, an empty field keeps the saved one
    char open[4] = "";
    form_field(body, "open", open, sizeof(open));
    if (strcmp(open, "1") == 0) {
        cfg.sta_password[0] = '\0';
    } else {
        int r = form_field(body, "password", value, sizeof(cfg.sta_password));
        if (r < 0) {
            return send_result(req, "The Wi-Fi password is too long");
        }
        if (r == 1 && value[0]) {
            if ((problem = bridge_config_check_sta_password(value))) {
                return send_result(req, problem);
            }
            strlcpy(cfg.sta_password, value, sizeof(cfg.sta_password));
        } else if (!cfg.sta_password[0]) {
            return send_result(req, "Enter the Wi-Fi password, or tick \"Open network\"");
        }
    }

    // PLC IP: empty = learn
    char ip_text[16];
    int r = form_field(body, "plc_ip", ip_text, sizeof(ip_text));
    if (r < 0) {
        return send_result(req, "Invalid PLC IP address");
    }
    if (r == 1) {
        if (ip_text[0] == '\0') {
            cfg.plc_ip = 0;
        } else if (!ipv4_parse(ip_text, &cfg.plc_ip)) {
            return send_result(req, "Invalid PLC IP address");
        }
    }

    // Setup AP password: empty keeps the saved one
    r = form_field(body, "setup_password", value, sizeof(cfg.setup_password));
    if (r < 0) {
        return send_result(req, "The setup password is too long");
    }
    if (r == 1 && value[0]) {
        if ((problem = bridge_config_check_setup_password(value))) {
            return send_result(req, problem);
        }
        strlcpy(cfg.setup_password, value, sizeof(cfg.setup_password));
    }

    esp_err_t err = bridge_config_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving settings failed: %s", esp_err_to_name(err));
        return send_result(req, "Saving failed");
    }
    ESP_LOGI(TAG, "Settings saved from the setup page - restarting");
    send_result(req, NULL);
    bridge_restart_soon();
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return send_result(req, "No OTA partition: flash the OTA partition table over USB first");
    }
    if (req->content_len == 0) {
        return send_result(req, "Empty upload");
    }
    if (req->content_len > target->size) {
        return send_result(req, "Image larger than the app partition");
    }
    ESP_LOGI(TAG, "Firmware upload: %u bytes to %s", (unsigned)req->content_len, target->label);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        return send_result(req, esp_err_to_name(err));
    }
    char *chunk = malloc(OTA_CHUNK_LEN);
    if (!chunk) {
        esp_ota_abort(ota);
        return send_result(req, "Out of memory");
    }
    size_t remaining = req->content_len;
    int timeouts = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, chunk, remaining < OTA_CHUNK_LEN ? remaining : OTA_CHUNK_LEN);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= OTA_MAX_TIMEOUTS) {
            continue;
        }
        if (n <= 0) {
            free(chunk);
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "Firmware upload interrupted");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota, chunk, (size_t)n);
        if (err != ESP_OK) {
            free(chunk);
            esp_ota_abort(ota);
            return send_result(req, esp_err_to_name(err));
        }
        remaining -= (size_t)n;
    }
    free(chunk);

    err = esp_ota_end(ota);                     // verifies the image checksum / hash
    if (err != ESP_OK) {
        return send_result(req, err == ESP_ERR_OTA_VALIDATE_FAILED ? "Not a valid ESP32 firmware image" : esp_err_to_name(err));
    }
    // Refuse images of other projects, e.g. an Arduino sketch or the bootloader.
    esp_app_desc_t new_app;
    if (esp_ota_get_partition_description(target, &new_app) != ESP_OK ||
            strcmp(new_app.project_name, esp_app_get_description()->project_name) != 0) {
        return send_result(req, "This image is not WT32 PLC Bridge firmware (expected build/wt32_eth2ap.bin)");
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return send_result(req, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Firmware %s written to %s - restarting", new_app.version, target->label);
    send_result(req, NULL);
    bridge_restart_soon();
    return ESP_OK;
}

static esp_err_t restart_post(httpd_req_t *req)
{
    send_result(req, NULL);
    bridge_restart_soon();
    return ESP_OK;
}

void bridge_web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;                   // config_post keeps a 1 KB form body on the stack
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed to start: %s", esp_err_to_name(err));
        return;
    }
    static const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_post },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &handlers[i]));
    }
}
