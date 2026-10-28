/*
 * WT32-ETH01 PLC bridge (Mode A): interface between the bridge core (bridge_main.c) and the
 * management side (web page, serial console).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // Links
    bool eth_link_up;
    bool wifi_connected;
    int rssi;                   // dBm, 0 when not connected
    uint8_t channel;            // 0 when not connected
    uint8_t bssid[6];
    // PLC
    bool plc_mac_known;
    bool plc_seen;              // confirmed on the wire since the last Ethernet link-up
    uint8_t plc_mac[6];
    uint32_t plc_ip;            // network byte order, 0 = not known yet
    // Frame counters
    uint32_t eth2wifi_fwd;
    uint32_t eth2wifi_queue_full;
    uint32_t eth2wifi_tx_fail;
    uint32_t wifi2eth_fwd;
    uint32_t wifi2eth_tx_fail;
    uint32_t reconnects;
    // Setup access point
    bool setup_ap_active;
    int setup_ap_clients;
} bridge_status_t;

void bridge_get_status(bridge_status_t *status);

// Opens or closes the setup access point. Safe to call from any task.
void bridge_request_setup_ap(bool on);

// Restarts in about a second, leaving time to finish an HTTP response or console output.
void bridge_restart_soon(void);

// Confirms a freshly updated firmware image so the bootloader won't roll it back.
void bridge_mark_app_valid(void);

// Management interfaces, started by bridge_main.c.
void bridge_web_start(void);
void bridge_console_start(void);
