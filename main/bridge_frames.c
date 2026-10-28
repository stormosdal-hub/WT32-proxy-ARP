/*
 * Ethernet / ARP frame helpers for the WT32-ETH01 PLC bridge.
 */
#include <stdio.h>
#include "bridge_frames.h"

#define ARP_HTYPE_ETHERNET  1
#define ARP_OP_REQUEST      1
#define ARP_PAYLOAD_LEN     28
#define IPV4_MIN_HDR_LEN    20

static inline uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

bool mac_is_link_local(const uint8_t *mac)
{
    static const uint8_t prefix[5] = { 0x01, 0x80, 0xC2, 0x00, 0x00 };
    return memcmp(mac, prefix, sizeof(prefix)) == 0 && (mac[5] & 0xF0) == 0;
}

uint16_t frame_ethertype(const uint8_t *frame, uint32_t len, uint32_t *payload_off)
{
    if (len < ETH_HDR_LEN) {
        return 0;
    }
    uint16_t type = read_be16(frame + 12);
    uint32_t off = ETH_HDR_LEN;
    if (type == ETHTYPE_VLAN) {
        if (len < ETH_HDR_LEN + 4) {
            return 0;
        }
        type = read_be16(frame + 16);
        off += 4;
    }
    if (payload_off) {
        *payload_off = off;
    }
    return type;
}

bool frame_is_profinet_dcp(const uint8_t *frame, uint32_t len)
{
    uint32_t off;
    if (frame_ethertype(frame, len, &off) != ETHTYPE_PROFINET || len < off + 2) {
        return false;
    }
    uint16_t frame_id = read_be16(frame + off);
    return frame_id >= 0xFEFC && frame_id <= 0xFEFF;
}

bool frame_sender_ip(const uint8_t *frame, uint32_t len, uint32_t *ip)
{
    if (len < ETH_HDR_LEN) {
        return false;
    }
    const uint8_t *payload = frame + ETH_HDR_LEN;
    switch (read_be16(frame + 12)) {
    case ETHTYPE_ARP:
        // Only IPv4-over-Ethernet ARP: htype 1, ptype 0x0800, hlen 6, plen 4. SPA at offset 14.
        if (len < ETH_HDR_LEN + ARP_PAYLOAD_LEN || read_be16(payload) != ARP_HTYPE_ETHERNET ||
                read_be16(payload + 2) != ETHTYPE_IPV4 || payload[4] != BR_MAC_LEN || payload[5] != 4) {
            return false;
        }
        memcpy(ip, payload + 14, 4);
        return true;
    case ETHTYPE_IPV4:
        if (len < ETH_HDR_LEN + IPV4_MIN_HDR_LEN || (payload[0] >> 4) != 4) {
            return false;
        }
        memcpy(ip, payload + 12, 4);
        return true;
    default:
        return false;
    }
}

size_t build_arp_request(uint8_t *buf, const uint8_t *src_mac, uint32_t sender_ip, uint32_t target_ip)
{
    memset(buf, 0, ETH_MIN_FRAME_LEN);
    memset(buf, 0xFF, BR_MAC_LEN);                      // destination: broadcast
    memcpy(buf + 6, src_mac, BR_MAC_LEN);
    write_be16(buf + 12, ETHTYPE_ARP);

    uint8_t *arp = buf + ETH_HDR_LEN;
    write_be16(arp, ARP_HTYPE_ETHERNET);
    write_be16(arp + 2, ETHTYPE_IPV4);
    arp[4] = BR_MAC_LEN;
    arp[5] = 4;
    write_be16(arp + 6, ARP_OP_REQUEST);
    memcpy(arp + 8, src_mac, BR_MAC_LEN);               // SHA
    memcpy(arp + 14, &sender_ip, 4);                    // SPA
    // THA (arp + 18) stays zero
    memcpy(arp + 24, &target_ip, 4);                    // TPA
    return ETH_MIN_FRAME_LEN;
}

bool ipv4_parse(const char *text, uint32_t *ip)
{
    unsigned a, b, c, d;
    char trailing;
    if (sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing) != 4 ||
            a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    const uint8_t bytes[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    memcpy(ip, bytes, 4);
    return true;
}
