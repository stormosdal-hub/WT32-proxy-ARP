/*
 * Persistent bridge settings. The Kconfig values are the factory defaults; anything saved from
 * the web page or the serial console is stored in NVS and takes precedence after a restart.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char sta_ssid[33];          // existing Wi-Fi network to join
    char sta_password[65];      // empty = open network
    uint32_t plc_ip;            // network byte order, 0 = learn from the PLC's traffic
    char setup_password[65];    // setup access point (WPA2, at least 8 characters)
} bridge_config_t;

// Fills cfg with the saved settings, falling back to the Kconfig defaults.
void bridge_config_load(bridge_config_t *cfg);

// Stores the values that differ from the saved settings (limits flash wear).
esp_err_t bridge_config_save(const bridge_config_t *cfg);

// Erases saved settings and the cached PLC MAC / Wi-Fi channel.
esp_err_t bridge_config_factory_reset(void);

// Validation shared by the web page and the console. Each returns NULL if valid, else a message.
const char *bridge_config_check_ssid(const char *ssid);
const char *bridge_config_check_sta_password(const char *password);
const char *bridge_config_check_setup_password(const char *password);
