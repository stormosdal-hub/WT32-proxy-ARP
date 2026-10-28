# Project Plan: WT32-ETH01 Wi-Fi ↔ Ethernet Bridge for Siemens S7-1200

**Revision 2 — 2026-10-17.** Supersedes Rev. 1. See [Appendix A](#appendix-a--changes-from-rev-1) for what changed.

---

## 1. Executive Summary & Objective

The goal is to connect a **Siemens S7-1200 PLC** to an existing plant or office Wi-Fi network using a **WT32-ETH01** (ESP32 + LAN8720A Ethernet PHY), without subnet isolation, NAT or port forwarding.

The PLC stays on the main LAN subnet (e.g. `192.168.1.0/24`) under its own IP. This enables **TIA Portal** online monitoring and downloads (S7 over ISO-on-TCP, TCP port `102`) and bidirectional **Open User Communication** (`TSEND_C`, `TRCV_C`, `TCON`) with no port mapping.

The work is staged so every step produces something usable:

1. **Stage 0: hardware bring-up with ESP-IDF's `eth2ap` example (Mode 0).** The ESP32 runs its own access point bridged to the PLC. This validates the board in about an hour. On its own it already gives a laptop full Layer-2 access to the PLC, including TIA Portal's "Accessible devices" scan.
2. **Mode A: single-device MAC cloning (primary target).** The ESP32 joins the plant Wi-Fi using the PLC's MAC address. Frames pass through unmodified.
3. **Mode B: MAC-NAT / proxy-ARP (optional).** Only needed if more than one Ethernet device must sit behind the bridge (e.g. PLC + HMI).

> **Gate:** the Wi-Fi infrastructure checks in [Section 3](#3-prerequisites-wi-fi-infrastructure-rf--security) must pass before firmware work starts. Several common WLAN settings block this design regardless of code quality.

---

## 2. Network Architecture & Protocol Theory

### 2.1 The 802.11 three-address limitation

In infrastructure mode, a frame sent from a station to the access point (ToDS) carries three MAC addresses:

1. **Receiver Address (RA):** the AP's BSSID.
2. **Transmitter / Source Address (TA = SA):** the station's own MAC.
3. **Destination Address (DA):** the final destination on the network.

There is no field for a separate source *behind* the station. The AP only accepts frames from associated stations, and only delivers frames to a station that are addressed to that station's own MAC. So if the ESP32 is associated under its own MAC:

- Frames forwarded with the PLC's MAC as source are not accepted as coming from the PLC.
- Frames addressed to the PLC's MAC are never delivered to the ESP32.

The standards-based fix is four-address (WDS) mode, but it is not available for an ESP32 station. This plan therefore uses one of the workarounds below.

### 2.2 Bridging strategies

**Mode 0: SoftAP bridge (`eth2ap`)**
- The ESP32 *is* the access point. An AP may send frames with any source address, and stations may address any destination. So no rewriting is needed and every EtherType passes.
- Trade-off: the laptop joins the ESP32's network instead of the plant Wi-Fi, so the PLC is *not* on the plant LAN.

**Mode A: single-device MAC cloning (recommended)**
- Learn the PLC's interface MAC ([Phase 3.2](#32-plc-mac-discovery)), then set it as the station MAC with `esp_wifi_set_mac(WIFI_IF_STA, plc_mac)` before `esp_wifi_start()`.
- Frames pass unmodified in both directions. There is no ARP table and no payload rewriting.
- Every PLC frame already carries the station MAC, so non-IP traffic such as PROFINET DCP (`0x8892`) can be forwarded too. This *may* let TIA Portal's "Accessible devices" scan work over Wi-Fi. It depends on the AP passing non-IP multicast and must be tested.

**Mode B: MAC-NAT / proxy-ARP (multiple devices)**
- The ESP32 keeps its own MAC and maintains an IP → MAC table for the devices on the Ethernet side.
- **Egress (Ethernet → Wi-Fi):** rewrite the Ethernet source MAC, and the ARP Sender Hardware Address (SHA), to the ESP32 station MAC.
- **Ingress (Wi-Fi → Ethernet):** map the destination IPv4 address, or the ARP Target Protocol Address (TPA), back to the device MAC. Rewrite the destination MAC, and the ARP Target Hardware Address (THA) where needed.
- IPv4 and ARP only. PROFINET DCP is not supported, so devices are always addressed by IP.

| | Mode 0: SoftAP (`eth2ap`) | Mode A: MAC clone | Mode B: MAC-NAT |
| :-- | :-- | :-- | :-- |
| PLC on plant Wi-Fi / LAN | No (laptop joins the ESP32's AP) | Yes | Yes |
| Ethernet devices behind bridge | One or more | Exactly one | Several |
| Frame rewriting | None | None | Ethernet header + ARP payload |
| PROFINET DCP / "Accessible devices" | Yes | Possibly (depends on the AP) | No |
| Ethernet device using DHCP | n/a | Works natively | Needs DHCP rewriting |
| MAC seen by the plant WLAN | none | PLC's MAC | ESP32's MAC |
| Starting point in ESP-IDF | `examples/network/eth2ap` | `eth2ap` / `sta2eth` plumbing | `examples/network/sta2eth` |
| Firmware complexity | Stock example | Low | Medium–high |

> **Reference code:** ESP-IDF's `sta2eth` example already bridges a Wi-Fi station to Ethernet using single-host MAC rewriting (Ethernet MACs + ARP SHA, optional DHCP rewriting). It learns the wired host's MAC **only from a DHCP Discover**, which a static-IP S7-1200 never sends, so it can't be used unchanged. Its receive/transmit plumbing is the right template for Modes A and B.

### 2.3 Target topologies

**Mode A / Mode B (PLC on the plant network):**
```text
[ Engineering PC / TIA Portal ]      [ External OUC Server / Client ]
       IP: 192.168.1.100                    IP: 192.168.1.200
               |                                    |
               +-----------------+------------------+
                                 | (LAN / WLAN subnet: 192.168.1.0/24)
                                 v
                      [ Plant/Office Wi-Fi AP ]
                          GW: 192.168.1.1
                                 |
                                 | (Wi-Fi station link, 2.4 GHz)
                                 v
                     [ WT32-ETH01 Bridge ]
                     (Mode A: MAC clone / Mode B: MAC-NAT)
                                 |
                                 | (RJ45 CAT5e/CAT6, 100 Mbps)
                                 v
                       [ Siemens S7-1200 PLC ]
                          IP: 192.168.1.50 / 255.255.255.0
                          Router: 192.168.1.1
```

If the engineering PC is also on Wi-Fi, client isolation must be off for that SSID (see [Section 3.1](#31-wlan-checklist)).

**Mode 0 (direct commissioning access):**
```text
[ Laptop / TIA Portal ]  ~~ Wi-Fi ~~  [ WT32-ETH01 SoftAP (eth2ap) ]  == RJ45 ==  [ S7-1200 ]
  static IP 192.168.1.100/24              no IP stack on the bridge                 192.168.1.50
```

---

## 3. Prerequisites: Wi-Fi Infrastructure, RF & Security

Clear these with the site IT/OT owner **before** implementation.

### 3.1 WLAN checklist

| Item | Why it matters | Requirement |
| :-- | :-- | :-- |
| 2.4 GHz SSID | The ESP32 radio is 2.4 GHz only | Target SSID available on 2.4 GHz at the PLC location (not 5 GHz-only or band-steered away from 2.4 GHz) |
| Client isolation (AP / peer-to-peer isolation) | Blocks traffic between two Wi-Fi clients, e.g. a laptop and the PLC on the same SSID | Disabled on this SSID, or the engineering PC on the wired LAN with routing allowed |
| DHCP enforcement / IP-theft detection | Settings like "DHCP Required" or "Enforce DHCP" drop clients with static IPs | Disabled for this SSID; the S7-1200 uses a static IP |
| MAC allow-list / NAC | Unknown MACs are denied | Register the PLC's MAC (Mode A) or the ESP32 station MAC (Mode B) |
| IP reservation | Avoid address conflicts | PLC IP outside the DHCP pool and documented |
| Authentication | WPA2/WPA3-Personal is simplest; 802.1X needs credentials or certificates on the ESP32 | WPA2/WPA3-Personal or WPA2-Enterprise (`esp_eap_client`). Captive portals are not supported |
| Non-IP multicast (Mode A DCP only) | Some WLANs drop non-IP frames | Optional; only affects "Accessible devices" |

### 3.2 RF & enclosure

- The WT32-ETH01 has a **PCB antenna**, and a closed steel cabinet blocks most of its signal. Mount the module in a plastic enclosure or outside the cabinet, or use a board variant with an external antenna connector.
- Survey the mounting location: aim for **RSSI ≥ −67 dBm** (−75 dBm absolute floor) and check channel congestion.
- The device is stationary. Prefer a location with one clearly strongest AP so it doesn't flap between APs.

### 3.3 Security

- **Network segmentation:** use a dedicated OT SSID/VLAN with a firewall that only lets engineering stations and OUC partners reach the PLC (TCP 102 plus the OUC ports).
- **PLC:** set an access protection level with a password, leave PUT/GET disabled unless required, and enable secure PG/PC communication where the CPU firmware supports it.
- **ESP32:** use a unique, strong password for the maintenance AP. Consider flash encryption, since Wi-Fi credentials are stored on the device. Accept OTA updates only through the authenticated maintenance interface.
- Get written approval from the site IT/OT owner before connecting a PLC to a shared WLAN.

---

## 4. Hardware Specification & Pinout (WT32-ETH01)

### 4.1 Power supply

- **Preferred:** 5 V DC (≥ 1 A) on the `5V` and `GND` pins; the onboard regulator produces 3.3 V.
- **Alternative:** 3.3 V directly on `3V3` from a stiff regulator (≥ 500 mA). The current peaks come from **Wi-Fi TX bursts** (several hundred mA), not from the Ethernet PHY, so a solid supply is all that's needed.
- **From the 24 V cabinet bus:** use an isolated 24 V → 5 V DC/DC converter (≥ 5 W) and add bulk capacitance (100–470 µF) close to the module.
- Keep the ESP32 brownout detector enabled (default).

### 4.2 ESP32 ↔ LAN8720A fixed pin mapping

| Signal | ESP32 GPIO | Function / Notes |
| :--- | :--- | :--- |
| `ETH_MDC` | `GPIO 23` | Management Data Clock |
| `ETH_MDIO` | `GPIO 18` | Management Data Input/Output |
| `ETH_RMII_CLK` | `GPIO 0` | 50 MHz external clock **input** (`EMAC_CLK_EXT_IN`). GPIO0 is also a boot strapping pin |
| `ETH_OSC_EN` | `GPIO 16` | Enables the 50 MHz oscillator. Must stay **low during reset** so GPIO0 strapping isn't disturbed. Drive high in firmware, then wait ~10 ms before Ethernet init |
| `ETH_TXD0` | `GPIO 19` | RMII Transmit Data 0 |
| `ETH_TXD1` | `GPIO 22` | RMII Transmit Data 1 |
| `ETH_TX_EN` | `GPIO 21` | RMII Transmit Enable |
| `ETH_RXD0` | `GPIO 25` | RMII Receive Data 0 |
| `ETH_RXD1` | `GPIO 26` | RMII Receive Data 1 |
| `ETH_CRS_DV` | `GPIO 27` | RMII Carrier Sense / Receive Data Valid |
| `PHY_ADDR` | `1` | PHY address |
| PHY reset | none (`-1`) | No PHY reset line is wired |

> In ESP-IDF v6.x, `ETH_ESP32_EMAC_DEFAULT_CONFIG()` for the ESP32 already uses MDC 23, MDIO 18, RMII, and the external clock input on GPIO0. Only the PHY address, the reset GPIO and the oscillator enable are board-specific.

### 4.3 Spare GPIO (maintenance button / status LED)

- Avoid the strapping pins (`IO0`, `IO2`, `IO12`, `IO15`) for the button.
- Suggested: maintenance button on `IO4` (input, internal pull-up); status LED on `IO32` (CFG) or `IO33` (485_EN).
- `IO35`, `IO36` and `IO39` are input-only.
- Check these against your board revision's header silkscreen.

### 4.4 UART flashing (3.3 V USB-to-TTL adapter)

- `WT32 TX0` → `Adapter RX`
- `WT32 RX0` → `Adapter TX`
- `WT32 GND` → `Adapter GND`
- `WT32 IO0` → hold at `GND` while resetting (pulse `EN` or power-cycle) to enter the bootloader; release for normal boot.
- Power the board from its own 5 V supply and share GND with the adapter. Many adapters can't supply the Wi-Fi TX peaks.

### 4.5 Environmental rating

The WT32-ETH01 is a hobby-grade module: no industrial temperature range, EMC certification or conformal coating. It is fine for the lab, commissioning and semi-permanent use. For permanent production installations, see [Section 9](#9-known-limitations--risks).

---

## 5. Phased Implementation Plan

### Phase 1: Development environment

1. **ESP-IDF version:** use **v6.1**, the current release in its service period, which Espressif recommends for new projects. **v5.5.2 or later** is an acceptable alternative if you follow older tutorials. The `espressif/ethernet_init` component rejects 5.5.0 and 5.5.1. v5.1 is past end of life and v5.2 has reached the end of its 30-month support window.
2. **Pin the exact version** (git tag, recorded in the project README). The bridge uses private Wi-Fi APIs (`esp_private/wifi.h`: `esp_wifi_internal_tx`, `esp_wifi_internal_reg_rxcb`, `esp_wifi_internal_free_rx_buffer`) that may change between minor releases. Re-test after every upgrade.
3. **Ethernet configuration in v6.x:**
   - The RMII Kconfig options (`ETH_PHY_INTERFACE_RMII`, `ETH_RMII_CLK_INPUT`, `ETH_RMII_CLK_IN_GPIO`, …) were removed. The clock is set in code via `eth_esp32_emac_config_t.clock_config.rmii` and the SMI pins via `.smi_gpio`.
   - `esp_eth_phy_new_lan87xx()` moved out of ESP-IDF. Add it with `idf.py add-dependency "espressif/lan87xx"` or use the `espressif/ethernet_init` component, which the examples use.
   - Keep `CONFIG_ETH_USE_ESP32_EMAC=y`.
   - On v5.5, field names differ slightly. Follow that version's `examples/ethernet/basic`.
4. **sdkconfig.** Larger Wi-Fi buffers absorb bursts and raise throughput; they do **not** reduce latency (that's what disabling power save does, see [Section 8](#8-reliability--edge-case-mitigations)):
   - `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16`
   - `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=64`
   - `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=64`
   - `CONFIG_FREERTOS_HZ=1000`, so the 1 ms back-off delays in the TX retry loop are real.
   - Raise `CONFIG_ETH_DMA_RX_BUFFER_NUM` / `CONFIG_ETH_DMA_TX_BUFFER_NUM` above their defaults if stress tests show Ethernet drops.
   - A partition table with two OTA app slots (4 MB flash), for [Phase 4](#phase-4-management-configuration--diagnostics).
   - `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.

### Phase 2: Hardware bring-up with `eth2ap` (Mode 0)

1. Create the project from `examples/network/eth2ap` of the pinned ESP-IDF version.
2. Ethernet settings (`ethernet_init` component, preset in this project's `sdkconfig.defaults`): internal EMAC, RMII with clock input on GPIO0, PHY LAN87xx, PHY address `1`, MDC `23`, MDIO `18`, PHY reset GPIO `-1`.
3. Oscillator enable: set `CONFIG_ETHERNET_BOARD_SPECIFIC_INIT_DEFAULT=y` and `CONFIG_ETHERNET_EXT_OSC_EN_GPIO_NUM=16`. `ethernet_init` then drives GPIO16 high in the EMAC low-level init hook, which runs before the EMAC reset; the reset waits up to 100 ms for the clock. No code change is needed.
4. Set the SoftAP SSID, WPA2 password and channel in menuconfig.
5. Know how the example behaves:
   - It starts the SoftAP **only after the Ethernet link comes up** and uses the ESP32's Ethernet MAC as the SoftAP MAC, so the PLC must be connected and powered.
   - There is no DHCP server on the bridge, so give the laptop a static IP in the PLC subnet (e.g. `192.168.1.100/24`).
   - It queues Ethernet → Wi-Fi traffic through a flow-control task. Tune `FLOW_CONTROL_QUEUE_LENGTH` and `FLOW_CONTROL_WIFI_SEND_DELAY_MS` if it logs send failures.
6. Verify (tests 1–2 in [Section 7.1](#71-functional-tests)): the link comes up at 100 Mbps full duplex, ping succeeds, "Accessible devices" finds the PLC, and go-online and download work.

**Exit criteria:** stable go-online and download in Mode 0. Keep this build as a direct commissioning tool.

### Phase 3: Mode A bridge (station + MAC clone)

> **Status (2026-10-17):** implemented in `main/bridge_main.c` and `main/bridge_frames.c`, built with ESP-IDF v6.1. Verified on hardware: through a GL.iNet GL-MT3000, TIA Portal goes online, monitors and downloads with the PLC at `192.168.8.50`. "Accessible devices" (DCP) also finds the PLC through the router. Robustness results:
> - **Ping:** 0 % loss over several hours; average ~5 ms, 2–33 ms.
> - **Recovery:** survives a router power cycle; after a bridge power cycle it is back on Wi‑Fi in ~3.5 s using the cached MAC.
> - **Load:** a large download had no TX failures; TIA Portal stayed online for many hours.
> - **Not yet run:** test 16 (PLC swap). Deviation from 3.5: the last channel is cached in RAM only (NVS caching of channel/BSSID moves to Phase 4).

#### 3.1 Initialisation order

1. `nvs_flash_init()`, `esp_event_loop_create_default()`.
2. Oscillator enable: GPIO16 high, ~10 ms delay.
3. Ethernet:
   - Create the EMAC and the LAN87xx PHY, then `esp_eth_driver_install()`.
   - Enable promiscuous mode with `ETH_CMD_S_PROMISCUOUS`. It is needed to receive frames sent **by** the PLC, which are addressed to other MACs (gateway, PC), not to the ESP32.
   - Register `esp_eth_update_input_path(eth, eth_to_wifi, NULL)`, then call `esp_eth_start()`.
4. PLC MAC discovery ([3.2](#32-plc-mac-discovery)). At boot, use the NVS-cached PLC MAC immediately so Wi-Fi doesn't wait for the PLC.
5. Wi-Fi station with the cloned MAC. **Do not call `esp_netif_create_default_wifi_sta()`**: its default handlers register lwIP's receive callback on the STA interface and would capture the bridged frames.

```c
// Ethernet (ESP-IDF v6.1 naming; LAN87xx driver from the espressif/lan87xx component)
gpio_reset_pin(GPIO_NUM_16);
gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
gpio_set_level(GPIO_NUM_16, 1);                       // enable 50 MHz oscillator
vTaskDelay(pdMS_TO_TICKS(10));

eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();  // ESP32 defaults already match WT32-ETH01
emac_cfg.smi_gpio.mdc_num  = 23;                      // explicit for clarity
emac_cfg.smi_gpio.mdio_num = 18;
emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
emac_cfg.clock_config.rmii.clock_gpio = 0;

eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
phy_cfg.phy_addr = 1;
phy_cfg.reset_gpio_num = -1;

esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);
```

```c
// Wi-Fi station, no esp_netif
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
ESP_ERROR_CHECK(esp_wifi_init(&cfg));
ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));      // credentials live in our own NVS namespace
ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, s_plc_mac));    // Mode A only; must precede esp_wifi_start()
ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));  // cached channel/BSSID for fast connect
ESP_ERROR_CHECK(esp_wifi_start());
ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
```

#### 3.2 PLC MAC discovery

- **Settings in NVS:** the PLC IP (optional, recommended) and the last learned PLC MAC.
- **Passive learning:** learn from IPv4 or ARP frames on the Ethernet side whose sender IP equals the configured PLC IP. If no IP is configured, use the first non-zero sender IP. **Ignore sender IP `0.0.0.0`**: those are ARP probes from the PLC's IP-conflict detection.
- **Learn the interface MAC only.** Siemens PROFINET interfaces use separate *port* MACs as the source of LLDP frames. The MAC to clone is the *interface* MAC used for IP traffic, so never learn from LLDP or other link-local frames.
- **Active discovery:** on Ethernet link-up with a PLC IP configured, send an ARP probe every 2 s until it is answered: SHA = the ESP32 Ethernet MAC, SPA = `0.0.0.0`, TPA = the PLC IP. SPA `0.0.0.0` avoids polluting the PLC's ARP cache.
  - Verify that your CPU firmware answers probes. If it doesn't, rely on passive learning; the PLC typically sends a gratuitous ARP after link-up.
- **PLC replaced:** if the learned MAC differs from the cloned MAC, write the new MAC to NVS, then run `esp_wifi_disconnect()` → `esp_wifi_stop()` → `esp_wifi_set_mac()` → `esp_wifi_start()`.
- **Commissioning:** cross-check the learned MAC against the PLC's label or TIA Portal *Online & diagnostics*.

#### 3.3 Forwarding rules

**Ethernet → Wi-Fi (egress).** Forward only if *all* of these hold:
- The station is connected.
- The source MAC equals the cloned PLC MAC.
- The destination MAC is not the ESP32's own Ethernet MAC. Replies to discovery probes are consumed locally.
- The destination is not IEEE link-local multicast `01:80:C2:00:00:0x` (LLDP etc.).
- The EtherType is in the allow-list:
  - `0x0800` IPv4
  - `0x0806` ARP
  - `0x8892` PROFINET, for DCP (configurable; experimental)

**Wi-Fi → Ethernet (ingress).**
- Forward if the destination MAC is the PLC MAC, broadcast or multicast.
- Optional, configurable filters:
  - Drop IPv6 (`0x86DD`); the S7-1200 doesn't use it.
  - Rate-limit broadcast/multicast to shield the PLC from office-network chatter (mDNS, SSDP, NetBIOS).

#### 3.4 Data path & buffer ownership

This follows the `eth2ap` pattern: a flow-control queue on the Ethernet → Wi-Fi side, and a direct transmit on the Wi-Fi → Ethernet side.

```c
typedef struct { uint8_t *buf; uint32_t len; } tx_msg_t;

// Ethernet → Wi-Fi. Runs in the EMAC RX task; we own `buf`.
static esp_err_t eth_to_wifi(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv)
{
    tx_msg_t msg = { buf, len };
    if (s_sta_connected && egress_allowed(buf, len) &&
        xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
        return ESP_OK;                   // wifi_tx_task frees it after sending
    }
    free(buf);                           // every drop path frees the buffer
    return ESP_OK;
}

// Drains the queue; backs off while Wi-Fi TX buffers are full (ESP_ERR_NO_MEM).
static void wifi_tx_task(void *arg)
{
    tx_msg_t msg;
    for (;;) {
        xQueueReceive(s_tx_queue, &msg, portMAX_DELAY);
        int err = esp_wifi_internal_tx(WIFI_IF_STA, msg.buf, msg.len);
        for (int delay_ms = 1; err != ESP_OK && delay_ms <= 32; delay_ms *= 2) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            err = esp_wifi_internal_tx(WIFI_IF_STA, msg.buf, msg.len);
        }
        if (err != ESP_OK) {
            s_stats.eth2wifi_drop++;     // TCP will retransmit
        }
        free(msg.buf);
    }
}

// Wi-Fi → Ethernet. Runs in the Wi-Fi task: don't block, always release `eb`.
static esp_err_t wifi_to_eth(void *buf, uint16_t len, void *eb)
{
    if (s_eth_link_up && ingress_allowed(buf, len)) {
        if (esp_eth_transmit(s_eth, buf, len) != ESP_OK) {
            s_stats.wifi2eth_drop++;
        }
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}
```

Rules:
- The Ethernet input callback owns `buf` and must `free()` it on **every** path, including drops and failures.
- `esp_wifi_internal_tx` copies the frame, so the buffer can be freed right after the call. `ESP_ERR_NO_MEM` means the Wi-Fi TX buffers are full: back off briefly (bounded, about 60 ms in total), then drop and count the frame.
- Keep the TX queue bounded (around 40 entries, as in `eth2ap`) and count queue-full drops.
- The Wi-Fi receive callback must end with `esp_wifi_internal_free_rx_buffer(eb)` on every path.

#### 3.5 Wi-Fi connection management

- **`WIFI_EVENT_STA_START`:** `esp_wifi_connect()`.
- **`WIFI_EVENT_STA_CONNECTED`:**
  - Register the receive callback with `esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_to_eth)`. Do this on *every* connect, as `sta2eth` does.
  - Set the connected flag.
  - Cache the channel and BSSID in NVS if they changed.
  - Send a gratuitous ARP ([Section 8](#8-reliability--edge-case-mitigations)).
- **`WIFI_EVENT_STA_DISCONNECTED`:**
  - Clear the connected flag.
  - Unregister the callback with `esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL)`.
  - Reconnect with backoff: immediately, then 1 s, 2 s, 5 s, capped at 5 s.
- **Fast reconnect:** use the cached channel (and optionally the BSSID) with `WIFI_FAST_SCAN`. Fall back to a full scan after 3 failed attempts.

**Exit criteria:** tests 3–9 ([Section 7.1](#71-functional-tests)) and 10, 12–16 ([Section 7.2](#72-robustness--soak-tests)) pass in Mode A.

### Phase 4: Management, configuration & diagnostics

> **Status (2026-10-17):** implemented (`bridge_config.c`, `bridge_web.c`, `bridge_console.c`, `web/index.html`, `partitions.csv`). It builds with ESP-IDF v6.1 but hasn't been tested on hardware. Differences from the list below:
> - **Settings:** saved settings are the Wi-Fi network/password, PLC IP and setup password. The bridge mode and the filter switches stay build-time options (Mode B doesn't exist yet). The Wi-Fi channel is cached; the BSSID isn't.
> - **Setup AP triggers:** the WT32-ETH01 has no spare button, so the AP opens for 10 min after every boot instead; also after 2 min without Wi-Fi, when no network is configured, and on `setup on`.
> - **Status LED:** not implemented (no LED on the board).
> - **OTA:** a new image is confirmed when the station connects or the setup page loads, not when Ethernet is up, because the PLC may legitimately be off.
> - **Default handlers (item 3 check):** verified in the v6.1 source. `wifi_default.c` touches the station only when a station netif exists, and Mode A never creates one.

1. **Configuration in NVS:**
   - SSID and passphrase, or EAP credentials.
   - PLC IP; the cached PLC MAC, channel and BSSID.
   - Bridge mode (A/B) and filter switches (DCP forwarding, IPv6 drop, broadcast limit).
   - Maintenance-AP password.
   - Write only when a value changes, to limit flash wear.
2. **Serial console** (`esp_console` on UART0): `wifi <ssid> <pass>`, `plc ip <addr>`, `mode a|b`, `status`, `counters`, `reboot`, `factory-reset`.
3. **Maintenance SoftAP**, on demand, in AP+STA mode:
   - **Triggers:** maintenance button held at boot, the station unable to connect for 5 min, or a console command. Switches off automatically after 15 min idle.
   - **Separation from the bridge:** own SSID and WPA2 password, plus a small lwIP netif with a DHCP server on the SoftAP's *own* MAC, independent of the bridged STA path.
   - **Serves** a status page, a configuration form and OTA upload.
   - In AP+STA mode the SoftAP must use the station's channel.
   - **Check:** creating the default AP netif installs ESP-IDF's default Wi-Fi event handlers. Confirm that they don't re-register a receive callback on the STA interface: the bridge counters must keep incrementing after the AP starts. If they do, register the bridge callback after them, or create the AP netif without the default handlers.
4. **Status & counters:**
   - Ethernet link and speed.
   - Wi-Fi state, RSSI, channel and BSSID.
   - Cloned/learned PLC MAC and IP.
   - Frames in each direction, and drops (TX `NO_MEM`, queue full, filtered).
   - Reconnect count, uptime, last reset reason (`esp_reset_reason()`), free heap.
5. **Status LED:**
   - Slow blink: Wi-Fi connecting.
   - Fast blink: no PLC link or MAC not yet known.
   - Solid: bridging.
   - Double blink: maintenance AP active.
6. **OTA:** two app slots, uploaded via the maintenance AP. With rollback enabled, mark the new image valid only after both the Wi-Fi and Ethernet links are up.

*Stretch goal:* also bridge the maintenance SoftAP to the PLC (`eth2ap`-style local access when the plant Wi-Fi is down). This needs three-port forwarding logic, so defer it.

### Phase 5: Mode B — MAC-NAT (optional)

Only needed if several Ethernet devices must share the bridge. The station keeps its own MAC; no `esp_wifi_set_mac()`. Use `sta2eth`'s `mac_spoof()` as a reference, but replace its DHCP-Discover-only host learning with the rules below.

#### 5.1 Translation table

- A small fixed table (e.g. 8 entries) of `{ IPv4, MAC, last_seen }`. Optional static entries from NVS never expire.
- Learned entries expire after 10 min without traffic.
- The table is written from the Ethernet RX path and read from the Wi-Fi RX path. Protect it with a spinlock or critical section: copy the entry out, and never hold the lock while transmitting.
- **Single device behind the bridge:** no IP lookup is needed. Rewrite every unicast frame whose destination MAC is the station MAC to the device MAC. This automatically covers unicast ARP requests.

#### 5.2 Ethernet → Wi-Fi

1. Accept EtherType `0x0800` and `0x0806` only; drop everything else, including DCP.
2. Learn `(sender IP, source MAC)` from ARP (SPA) and IPv4 (source IP). Skip `0.0.0.0`.
3. Rewrite the Ethernet source MAC (bytes 6–11) to the station MAC.
4. ARP: rewrite the SHA (frame offset 22) to the station MAC. This includes probes.
5. DHCP from a device (UDP 68 → 67), only if a device uses DHCP:
   - Set the BOOTP broadcast flag (`0x8000`) so replies come back as broadcasts.
   - Then set the UDP checksum to `0` (allowed for UDP over IPv4) or update it incrementally.
   - `chaddr` still carries the device MAC, and WLANs with DHCP snooping may reject the mismatch. `sta2eth` instead rewrites `chaddr` and option 61 and recomputes the checksum.
6. Transmit with the same queue and back-off rules as Mode A.

#### 5.3 Wi-Fi → Ethernet

1. **Broadcast / multicast:** forward unchanged, subject to the filters.
2. **Unicast IPv4:** look up the destination IP (frame offset 30).
   - Hit: rewrite the destination MAC.
   - Miss: drop it, or hand it to the management netif ([5.4](#54-optional-management-ip)).
3. **ARP whose TPA (frame offset 38) is in the table:**
   - **Unicast destination MAC:** rewrite it to the device MAC. This covers ARP replies *and* **unicast ARP requests**. Windows and Linux revalidate cached entries with unicast requests sent to the cached (ESP32) MAC. If they aren't rewritten, the device drops them, the peer's entry goes stale, and traffic stalls periodically.
   - **THA (frame offset 32) equal to the station MAC:** rewrite it to the device MAC.
   - **Broadcast requests:** forward unchanged. The device answers itself, and its reply is rewritten on egress.
4. **ARP whose TPA is not in the table:** forward broadcasts, drop unicasts.
5. Transmit as in Mode A.

#### 5.4 Optional management IP

The ESP32 can own an IP on its station MAC:
- Frames whose destination IP, or ARP TPA, equals the management IP go to an lwIP netif (custom `esp_netif` driver).
- Everything else goes to Ethernet.

This makes the maintenance AP unnecessary in Mode B.

**Exit criteria:** all tests in [Section 7](#7-verification--testing) pass in Mode B, including the ARP refresh test (11).

---

## 6. Siemens S7-1200 & TIA Portal Configuration

### 6.1 PLC IP configuration (once, wired, if not already set)

- **IP address:** static, inside the Wi-Fi subnet and outside the DHCP pool (e.g. `192.168.1.50`).
- **Subnet mask:** `255.255.255.0`.
- **Router / gateway:** the main router (e.g. `192.168.1.1`).
- Record the PLC's **interface MAC** (from the label or TIA *Online & diagnostics*) to verify [Phase 3.2](#32-plc-mac-discovery).

### 6.2 Connecting in TIA Portal

1. Open *Go online* or *Extended download to device*. Interface type `PN/IE`; select the laptop's adapter (Wi-Fi, or the wired NIC if the PC is on the wired LAN).
2. **Mode 0, and possibly Mode A:** "Show all compatible devices" (DCP scan) should list the PLC.
3. **Mode B, or if the scan finds nothing:** enter `192.168.1.50` directly as the target device address.
4. "Assign IP address" and "Assign PROFINET device name" depend on DCP. Do them over a wired connection or in Mode 0.

### 6.3 Open User Communication (OUC)

In the `TCON_IP_v4` data block:
- **Active mode:** set `RemoteAddress` to the partner IP (e.g. `192.168.1.200`).
- **Passive mode:** leave `RemoteAddress` at `0.0.0.0` (or the partner IP) and set `LocalPort` (e.g. `2000`). External clients connect to `192.168.1.50:2000`.
- If OUC partners are on another VLAN or subnet, the firewall must permit those ports ([Section 3.3](#33-security)).

### 6.4 PLC security settings

Apply [Section 3.3](#33-security): an access protection level with a password, PUT/GET disabled unless required, and secure PG/PC communication where supported.

---

## 7. Verification & Testing

### 7.1 Functional tests

| # | Test | Command / Action | Expected Result |
| :- | :--- | :--- | :--- |
| 1 | Ethernet link | Connect WT32-ETH01 ↔ S7-1200 and power up | RJ45 LEDs active; UART logs link up, 100 Mbps full duplex |
| 2 | Mode 0 bring-up | Laptop on ESP32 SoftAP (static IP); TIA "Accessible devices" | PLC listed; go-online and download work |
| 3 | WLAN association (A/B) | AP/controller client list; bridge status page | Mode A: client shown with the **PLC's MAC**; Mode B: with the **ESP32 MAC**. RSSI ≥ −67 dBm |
| 4 | ARP resolution | Admin shell: `arp -d *`, then `ping 192.168.1.50`, then `arp -a` | Ping succeeds. Mode A: PLC IP ↔ PLC MAC; Mode B: PLC IP ↔ ESP32 station MAC |
| 5 | S7 port 102 | `Test-NetConnection 192.168.1.50 -Port 102` | `TcpTestSucceeded : True` |
| 6 | TIA online & download | Go online, monitor OB1, download a DB/FC | Green checkmarks; live monitoring active |
| 7 | DCP over Wi-Fi (Mode A, optional) | TIA "Accessible devices" via the Wi-Fi adapter | PLC listed. If not, the AP is filtering non-IP multicast; enter the IP manually |
| 8 | OUC passive socket | `nc 192.168.1.50 2000` (or Packet Sender) | `TRCV_C` `RCVD_LEN` increments; payload intact |
| 9 | OUC active socket | Trigger `TSEND_C` `REQ` towards a PC listener | PC receives the stream from source IP `192.168.1.50` |

### 7.2 Robustness & soak tests

The latency and recovery figures are starting targets; adjust them after a baseline measurement.

| # | Test | Method | Pass Criteria |
| :- | :--- | :--- | :--- |
| 10 | Loss & jitter | `ping -n 1000 192.168.1.50` | 0 % loss; avg < 10 ms, max < 100 ms |
| 11 | ARP refresh (Mode B) | Low-rate ping (every 5 s) for 30 min from a Windows *and* a Linux host; watch `ip neigh` on Linux | No gaps when entries go stale and are revalidated |
| 12 | Large download | Full download of the largest project (big DBs) | Completes; `NO_MEM` / queue-full counters stay low |
| 13 | AP outage | Reboot the AP, or disable the SSID for 60 s | Reconnects and sends a gratuitous ARP; ping resumes < 5 s after the AP is back |
| 14 | PLC power cycle | Power-cycle the PLC | MAC confirmed; traffic resumes without an ESP32 reboot |
| 15 | Bridge power cycle | Power-cycle the WT32-ETH01 | Boots with the cached MAC; bridging within ~10 s |
| 16 | PLC swap (Mode A) | Replace the CPU with one that has a different MAC | New MAC detected, Wi-Fi restarted with the new clone, traffic resumes |
| 17 | Broadcast load | 24 h on the real office network | PLC cycle time unaffected; enable the broadcast limit if it is |
| 18 | Soak | 24 h TIA online monitoring + continuous ping | No ESP32 resets (check uptime and reset reason); free heap stable; monitoring drops only during Wi-Fi events |

---

## 8. Reliability & Edge-Case Mitigations

1. **Disable Wi-Fi power save (critical for latency).**
   - Call `esp_wifi_set_ps(WIFI_PS_NONE)` right after `esp_wifi_start()`.
   - Default modem sleep (`WIFI_PS_MIN_MODEM`) adds latency spikes of roughly 100–300 ms (DTIM-dependent), which can break TIA online monitoring.
   - Note: `sta2eth` leaves power save at its default.
2. **Reconnect quickly.**
   - Use the backoff from [Phase 3.5](#35-wi-fi-connection-management).
   - Use the cached channel/BSSID with `WIFI_FAST_SCAN`, falling back to a full scan.
3. **Send a gratuitous ARP** after every (re)connect and after a PLC MAC change. It is an ARP request with `SPA == TPA == device IP`, sent to broadcast:
   - Mode A: `SHA` = the PLC MAC.
   - Mode B: `SHA` = the station MAC, one per table entry.

   This updates the ARP caches of the engineering PC and the gateway immediately.
4. **Backpressure:**
   - Ethernet → Wi-Fi: a bounded queue with bounded TX back-off.
   - Wi-Fi → Ethernet: a direct transmit that never blocks.
   - Count every drop.
5. **Watchdogs:**
   - Subscribe the TX task to the task watchdog (`esp_task_wdt`).
   - Connectivity watchdog: `esp_restart()` if Wi-Fi hasn't been connected for 10 min. Don't trigger it on Ethernet link loss; the PLC may legitimately be off.
   - Log the reset reason.
6. **AP selection:**
   - The device is stationary. An optional BSSID lock prevents flapping between APs but removes failover if that AP dies.
   - Default: off.
7. **Flash wear:** write NVS only when a value changes.
8. **Power integrity:** keep the brownout detector on and add bulk capacitance ([Section 4.1](#41-power-supply)).
9. **Mode B concurrency:** lock the table as described in [Phase 5.1](#51-translation-table).

---

## 9. Known Limitations & Risks

- **TIA Portal sessions:** online sessions will likely drop during Wi-Fi outages longer than a few seconds; go online again after the reconnect.
- **Throughput:** far below wired 100 Mbit/s. ESP32 Wi-Fi typically manages roughly 10–20 Mbit/s in practice. That is plenty for S7 traffic; large downloads just take longer.
- **Private API:** `esp_wifi_internal_*` can change between ESP-IDF releases. Pin the version and re-test after upgrades.
- **DCP:** in Mode A it depends on the AP; in Mode B it doesn't work. "Assign IP / device name" needs a wired connection or Mode 0.
- **Radio:** the ESP32 is 2.4 GHz only.
- **Mode A** supports exactly one Ethernet device.
- **Hardware:** the WT32-ETH01 is not industrial-rated (temperature range, EMC, certification). For permanent production use, an industrial Wi-Fi client (e.g. Siemens SCALANCE W) is the off-the-shelf alternative.
- **Security:** a PLC on a shared WLAN expands the attack surface. [Section 3.3](#33-security) is not optional.

---

## Appendix A — Changes from Rev. 1

- **Staging:** added Stage 0 / Mode 0 (`eth2ap`) as hardware bring-up and a commissioning fallback.
- **Order of work:** the phases now build **Mode A first** (Phase 3); Mode B moved to the optional Phase 5. Rev. 1 recommended Mode A but only specified Mode B.
- **ESP-IDF examples:** `eth2ap` and `sta2eth` are now named as starting points. `sta2eth` learns the host MAC only from DHCP Discover, so it doesn't work unchanged with a static-IP PLC.
- **Mode A forwarding:** forwards an EtherType allow-list that includes PROFINET DCP, instead of IPv4/ARP only.
- **MAC discovery:**
  - Learn from IP/ARP only, not LLDP or port MACs.
  - Ignore ARP probes with sender `0.0.0.0`.
  - Active ARP-probe discovery plus an NVS cache.
  - Handle a replaced PLC.
- **Mode B fixes:**
  - Unicast ARP requests now get their destination MAC rewritten; the THA rewrite was generalised.
  - Single-device shortcut.
  - Table locking and aging.
  - DHCP checksum options; `chaddr` caveat.
- **New Section 3:** WLAN prerequisites (2.4 GHz, client isolation, DHCP enforcement, MAC allow-lists), RF and enclosure, OT security.
- **ESP-IDF:**
  - v6.1 (or v5.5.x) instead of v5.1/v5.2, with the exact version pinned.
  - v6.x moves the RMII clock settings from Kconfig into `eth_esp32_emac_config_t` and moves the LAN87xx driver to a managed component.
  - Wi-Fi buffer settings are described as raising throughput, not reducing latency.
- **API corrections:**
  - No default STA netif in either mode.
  - Free the Ethernet RX buffer on every path.
  - Bounded queue with `NO_MEM` back-off on the Ethernet → Wi-Fi side (the `eth2ap` pattern).
  - Register the RX callback on every connect and unregister it on disconnect.
  - `esp_wifi_set_mac()` before `esp_wifi_start()`.
  - Corrected the reason promiscuous mode is needed.
- **New Phase 4:** NVS configuration, serial console, on-demand maintenance AP, diagnostics, OTA with rollback.
- **Reliability:** fast reconnect (cached channel/BSSID), backoff, connectivity watchdog, flash-wear note.
- **Testing:** mode-aware expected results; new robustness and soak tests (10–18).
- **Hardware notes:**
  - Corrected the power note: peaks come from Wi-Fi TX, not the PHY.
  - Added the GPIO0 strapping / GPIO16 timing note and spare-GPIO guidance.
  - Added antenna, enclosure and hardware-rating notes.
