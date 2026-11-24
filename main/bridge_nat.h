/*
 * MAC-NAT for other devices on the switch (Mode A, "bridge other devices").
 *
 * The station joins Wi-Fi under the PLC's MAC, the only MAC the access point knows. IPv4 frames
 * from other devices on the switch are sent under that MAC too (Ethernet source and ARP sender
 * address rewritten). Frames coming back are given the device's own MAC again, chosen by
 * destination IP. Anything that doesn't match a known device still goes to the PLC unchanged.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAT_MAX_DEVICES     16          // upper bound of CONFIG_BRIDGE_NAT_MAX_DEVICES

typedef struct {
    uint8_t mac[6];
    uint32_t ip;                        // network byte order
    uint32_t to_wifi;                   // frames sent to Wi-Fi since boot
    uint32_t to_eth;                    // frames delivered from Wi-Fi since boot
    int32_t idle_s;                     // seconds since its last frame, -1 = not seen since boot
} nat_device_t;

// Allocates the table and restores the devices saved in NVS.
void nat_init(int max_devices);

// Writes the table to NVS if it changed. Not from an RX path: flash writes block.
void nat_save_if_changed(void);

// EMAC RX task: `mac` sent a frame from `ip`. The caller excludes the PLC.
void nat_learn(const uint8_t *mac, uint32_t ip);

// EMAC RX task: prepares a frame from another device for Wi-Fi by giving it `station_mac` as
// source (and ARP sender) address. Returns false if the frame must not go to Wi-Fi.
bool nat_egress(uint8_t *frame, uint32_t len, const uint8_t *station_mac);

// Wi-Fi RX task: `frame` is unicast to `station_mac`. If its destination IP (or ARP target IP)
// belongs to another device, rewrites the destination MAC (and ARP target MAC) and returns true.
bool nat_ingress(uint8_t *frame, uint16_t len, const uint8_t *station_mac);

// Copies the table; returns the number of devices.
size_t nat_get_devices(nat_device_t *out, size_t max);
