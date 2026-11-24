/*
 * PROFINET IO devices on the Wi-Fi side (CONFIG_BRIDGE_FORWARD_PNIO).
 *
 * The PLC's cyclic data and alarm frames (PROFINET, but not DCP) go to Wi-Fi only for devices
 * that sent the PLC PROFINET frames over Wi-Fi recently. A device does that when it answers the
 * PLC's DCP Identify, before the connection is set up. The PLC's traffic with IO devices on its
 * own cable stays off the air.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PNIO_MAX_PEERS      8

// `timeout_s`: how long a device counts as present after its last PROFINET frame.
void pnio_init(int timeout_s);

// Wi-Fi RX task: `mac` sent a unicast PROFINET frame to the PLC.
void pnio_peer_seen(const uint8_t *mac);

// EMAC RX task: is `mac` an IO device on the Wi-Fi side?
bool pnio_peer_known(const uint8_t *mac);

int pnio_peer_count(void);

// A device that appeared (or came back) since the last call, for the log. Returns false if none.
bool pnio_take_new_peer(uint8_t mac[6]);
