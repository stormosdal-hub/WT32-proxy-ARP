/*
 * Persistent bridge settings (NVS namespace "bridge_cfg").
 */
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "bridge_config.h"
#include "bridge_frames.h"

static const char *TAG = "bridge_cfg";

#define CFG_NAMESPACE       "bridge_cfg"
#define CACHE_NAMESPACE     "bridge"            // cached PLC MAC and Wi-Fi channel (bridge_main.c)
#define KEY_SSID            "ssid"
#define KEY_PASSWORD        "pass"
#define KEY_PLC_IP          "plc_ip"
#define KEY_SETUP_PASSWORD  "setup_pass"

static void load_str(nvs_handle_t nvs, const char *key, char *dst, size_t size)
{
    char buf[65];
    size_t len = sizeof(buf);
    if (nvs_get_str(nvs, key, buf, &len) == ESP_OK) {
        strlcpy(dst, buf, size);
    }
}

void bridge_config_load(bridge_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->sta_ssid, CONFIG_BRIDGE_STA_SSID, sizeof(cfg->sta_ssid));
    strlcpy(cfg->sta_password, CONFIG_BRIDGE_STA_PASSWORD, sizeof(cfg->sta_password));
    strlcpy(cfg->setup_password, CONFIG_BRIDGE_SETUP_AP_PASSWORD, sizeof(cfg->setup_password));
    if (strlen(CONFIG_BRIDGE_PLC_IP) > 0 && !ipv4_parse(CONFIG_BRIDGE_PLC_IP, &cfg->plc_ip)) {
        ESP_LOGE(TAG, "Invalid CONFIG_BRIDGE_PLC_IP \"%s\" - learning the PLC IP instead", CONFIG_BRIDGE_PLC_IP);
        cfg->plc_ip = 0;
    }

    nvs_handle_t nvs;
    if (nvs_open(CFG_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;                                 // nothing saved yet
    }
    load_str(nvs, KEY_SSID, cfg->sta_ssid, sizeof(cfg->sta_ssid));
    load_str(nvs, KEY_PASSWORD, cfg->sta_password, sizeof(cfg->sta_password));
    load_str(nvs, KEY_SETUP_PASSWORD, cfg->setup_password, sizeof(cfg->setup_password));
    uint32_t ip;
    if (nvs_get_u32(nvs, KEY_PLC_IP, &ip) == ESP_OK) {
        cfg->plc_ip = ip;
    }
    nvs_close(nvs);
}

esp_err_t bridge_config_save(const bridge_config_t *cfg)
{
    bridge_config_t current;
    bridge_config_load(&current);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (err == ESP_OK && strcmp(cfg->sta_ssid, current.sta_ssid) != 0) {
        err = nvs_set_str(nvs, KEY_SSID, cfg->sta_ssid);
    }
    if (err == ESP_OK && strcmp(cfg->sta_password, current.sta_password) != 0) {
        err = nvs_set_str(nvs, KEY_PASSWORD, cfg->sta_password);
    }
    if (err == ESP_OK && strcmp(cfg->setup_password, current.setup_password) != 0) {
        err = nvs_set_str(nvs, KEY_SETUP_PASSWORD, cfg->setup_password);
    }
    if (err == ESP_OK && cfg->plc_ip != current.plc_ip) {
        err = nvs_set_u32(nvs, KEY_PLC_IP, cfg->plc_ip);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t erase_namespace(const char *ns)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t bridge_config_factory_reset(void)
{
    esp_err_t err = erase_namespace(CFG_NAMESPACE);
    if (err == ESP_OK) {
        err = erase_namespace(CACHE_NAMESPACE);
    }
    return err;
}

const char *bridge_config_check_ssid(const char *ssid)
{
    size_t len = strlen(ssid);
    return (len >= 1 && len <= 32) ? NULL : "The network name must be 1-32 characters";
}

const char *bridge_config_check_sta_password(const char *password)
{
    size_t len = strlen(password);
    return (len == 0 || (len >= 8 && len <= 63)) ? NULL : "The Wi-Fi password must be 8-63 characters (or empty for an open network)";
}

const char *bridge_config_check_setup_password(const char *password)
{
    size_t len = strlen(password);
    return (len >= 8 && len <= 63) ? NULL : "The setup password must be 8-63 characters";
}
