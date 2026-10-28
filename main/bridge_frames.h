/*
 * Ethernet / ARP frame helpers for the WT32-ETH01 PLC bridge.
 *
 * IPv4 addresses are kept as uint32_t in network byte order (the four bytes as they
 * appear on the wire); print them with IPV4_FMT / IPV4_ARGS.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BR_MAC_LEN          6
#define ETH_HDR_LEN         14
#define ETH_MIN_FRAME_LEN   60      // without FCS; shorter frames are zero-padded

#define ETHTYPE_IPV4        0x0800
#define ETHTYPE_ARP         0x0806
#define ETHTYPE_VLAN        0x8100
#define ETHTYPE_IPV6        0x86DD
#define ETHTYPE_PROFINET    0x8892

#define IPV4_FMT            "%u.%u.%u.%u"
#define IPV4_ARGS(ip)       ((const uint8_t *)&(ip))[0], ((const uint8_t *)&(ip))[1], \
                            ((const uint8_t *)&(ip))[2], ((const uint8_t *)&(ip))[3]

static inline bool mac_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, BR_MAC_LEN) == 0;
}

// Broadcast or multicast destination.
static inline bool mac_is_group(const uint8_t *mac)
{
    return mac[0] & 0x01;
}

// IEEE 802.1D link-local group addresses 01:80:C2:00:00:00..0F (LLDP, STP, ...), never bridged.
bool mac_is_link_local(const uint8_t *mac);

// EtherType after skipping one 802.1Q tag. Returns 0 for runt frames. If payload_off is
// not NULL it receives the offset of the payload that follows the EtherType.
uint16_t frame_ethertype(const uint8_t *frame, uint32_t len, uint32_t *payload_off);

// PROFINET DCP (Identify, Get/Set, Hello): FrameID 0xFEFC..0xFEFF. Cyclic RT and alarm
// frames share the EtherType but must not be bridged.
bool frame_is_profinet_dcp(const uint8_t *frame, uint32_t len);

// Sender IPv4 address of an untagged ARP (SPA) or IPv4 (source address) frame.
// Returns false for anything else.
bool frame_sender_ip(const uint8_t *frame, uint32_t len, uint32_t *ip);

// Builds a broadcast ARP request into buf (at least ETH_MIN_FRAME_LEN bytes) and returns its
// length. sender_ip == 0 gives an ARP probe; sender_ip == target_ip a gratuitous ARP.
size_t build_arp_request(uint8_t *buf, const uint8_t *src_mac, uint32_t sender_ip, uint32_t target_ip);

// Parses dotted-quad text ("192.168.8.50"). Returns false on malformed input.
bool ipv4_parse(const char *text, uint32_t *ip);
