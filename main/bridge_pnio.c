/*
 * PROFINET IO devices on the Wi-Fi side: see bridge_pnio.h.
 *
 * Written from the Wi-Fi RX task, read from the EMAC RX task: every access holds a spinlock for
 * a few dozen instructions.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "bridge_frames.h"
#include "bridge_pnio.h"

static struct {
    uint8_t mac[BR_MAC_LEN];
    int64_t seen_us;                    // 0 = free
} s_peers[PNIO_MAX_PEERS];
static int64_t s_timeout_us;
static int s_new_peer = -1;             // index to log, or -1
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// Callers hold s_lock.
static bool fresh(int i, int64_t now)
{
    return s_peers[i].seen_us != 0 && now - s_peers[i].seen_us < s_timeout_us;
}

void pnio_init(int timeout_s)
{
    s_timeout_us = (int64_t)timeout_s * 1000000;
}

void pnio_peer_seen(const uint8_t *mac)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    int slot = 0;
    for (int i = 0; i < PNIO_MAX_PEERS; i++) {
        if (s_peers[i].seen_us != 0 && mac_equal(s_peers[i].mac, mac)) {
            if (!fresh(i, now)) {
                s_new_peer = i;         // back after a while: log it again
            }
            s_peers[i].seen_us = now;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        if (s_peers[i].seen_us < s_peers[slot].seen_us) {
            slot = i;                   // free (0) or least recently seen
        }
    }
    memcpy(s_peers[slot].mac, mac, BR_MAC_LEN);
    s_peers[slot].seen_us = now;
    s_new_peer = slot;
    portEXIT_CRITICAL(&s_lock);
}

bool pnio_peer_known(const uint8_t *mac)
{
    int64_t now = esp_timer_get_time();
    bool known = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < PNIO_MAX_PEERS && !known; i++) {
        known = fresh(i, now) && mac_equal(s_peers[i].mac, mac);
    }
    portEXIT_CRITICAL(&s_lock);
    return known;
}

int pnio_peer_count(void)
{
    int64_t now = esp_timer_get_time();
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < PNIO_MAX_PEERS; i++) {
        n += fresh(i, now);
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

bool pnio_take_new_peer(uint8_t mac[6])
{
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    if (s_new_peer >= 0) {
        memcpy(mac, s_peers[s_new_peer].mac, BR_MAC_LEN);
        s_new_peer = -1;
        found = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}
