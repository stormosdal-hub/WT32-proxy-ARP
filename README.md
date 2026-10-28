# WT32-ETH01 PLC Bridge

Wireless access to a Siemens S7-1200 through a WT32-ETH01 (ESP32 + LAN8720A), following the [implementation plan](wt32_eth01_proxy_arp_l2_bridge_implementation_plan.md). Built with ESP-IDF **v6.1**. One setting chooses the mode: *menuconfig → WT32 PLC Bridge → Bridge mode*.

| Mode | What it does | Laptop joins | Plan phase |
| :-- | :-- | :-- | :-- |
| **A: MAC clone** (default) | The ESP32 joins your existing Wi-Fi **using the PLC's MAC address**, so the PLC appears on that network like any other client. Frames pass through unmodified. A separate setup access point serves status, settings and firmware updates. | Your normal Wi-Fi network | Phases 3 (verified 2026-10-17) and 4 |
| **0: own access point** | ESP-IDF's `eth2ap` example, unchanged: the ESP32 runs its own WPA2 network bridged to the RJ45 port. | The bridge's network, with a static IP | Phase 2 (verified 2026-10-17) |

## Files

| File | Contents |
| :-- | :-- |
| `main/bridge_main.c` | Mode A: PLC MAC discovery, station MAC cloning, forwarding in both directions, reconnects, setup access point, watchdog, statistics |
| `main/bridge_frames.c` / `.h` | Mode A: Ethernet/ARP frame parsing and building |
| `main/bridge_config.c` / `.h` | Mode A: settings saved in NVS (Wi-Fi network, PLC IP, setup password) |
| `main/bridge_web.c`, `main/web/index.html` | Mode A: setup page and its JSON API, firmware upload |
| `main/bridge_console.c` | Mode A: serial console commands |
| `partitions.csv` | Two 1.9 MB app slots for firmware updates over Wi-Fi |
| `main/ethernet_example_main.c` | Mode 0: verbatim from ESP-IDF v6.1 `examples/network/eth2ap`, so you can diff it against upstream |
| `main/Kconfig.projbuild` | Mode selection and the options for each mode |
| `main/CMakeLists.txt` | Compiles only the selected mode's sources |
| `main/idf_component.yml` | `espressif/ethernet_init ^1.4.1`, which pulls in the LAN87xx PHY driver on IDF 6.x |
| `sdkconfig.defaults` | WT32-ETH01 board settings, mode settings, Wi-Fi buffers, 1 ms FreeRTOS tick, 4 MB flash |

## WT32-ETH01 settings

| Option | Value | Why |
| :-- | :-- | :-- |
| `CONFIG_ETHERNET_PHY_INTERFACE_RMII` / `CONFIG_ETHERNET_RMII_CLK_INPUT` | y | LAN8720A over RMII, 50 MHz clock fed from the board's oscillator |
| `CONFIG_ETHERNET_RMII_CLK_GPIO` | 0 | Clock input pin (also a boot strapping pin) |
| `CONFIG_ETHERNET_PHY_LAN87XX` | y | LAN8720A PHY |
| `CONFIG_ETHERNET_PHY_ADDR` | 1 | Fixed by the board's strapping |
| `CONFIG_ETHERNET_MDC_GPIO` / `CONFIG_ETHERNET_MDIO_GPIO` | 23 / 18 | Management interface |
| `CONFIG_ETHERNET_PHY_RST_GPIO` | -1 | No PHY reset line |
| `CONFIG_ETHERNET_BOARD_SPECIFIC_INIT_DEFAULT` + `CONFIG_ETHERNET_EXT_OSC_EN_GPIO_NUM` | y / 16 | `ethernet_init` drives GPIO16 high in the EMAC low-level init hook, before the EMAC reset. The oscillator stays off during boot, so GPIO0 strapping works |
| `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` | y | The module has 4 MB of flash |

## Requirements

- **ESP-IDF v6.1.** ESP-IDF **5.5.0 and 5.5.1 will not work**: `espressif/ethernet_init` rejects them during dependency resolution.
- A 3.3 V USB-to-TTL adapter, and a 5 V (≥ 1 A) supply for the board.

### On this PC

- **Install:** ESP-IDF v6.1 is at `C:\Users\Storm\esp\v6.1\esp-idf`, next to 5.5.1. It shares the tools folder `C:\Users\Storm\.espressif` and has its own Python environment, `idf6.1_py3.11_env`.
- **Plain PowerShell terminal:** put Espressif's bundled Python 3.11 first on `PATH` before running `export.ps1`. Otherwise the script picks up the system Python 3.14 and fails:

  ```powershell
  $env:IDF_TOOLS_PATH = "C:\Users\Storm\.espressif"; $env:PATH = "C:\Users\Storm\.espressif\tools\idf-python\3.11.2;$env:PATH"; . C:\Users\Storm\esp\v6.1\esp-idf\export.ps1
  ```

- **VS Code:** this project's `.vscode/settings.json` selects the v6.1 setup (registered in `C:\Users\Storm\.espressif\esp_idf.json`).
- **Last verified builds (2026-10-17):** ESP-IDF 6.1.0, `ethernet_init` 1.4.1, `lan87xx` 1.1.0 (see `dependencies.lock`). Mode A app 0xE9A40 bytes (51 % of a 1.9 MB app slot free), Mode 0 app 0xAE170 bytes.

## Changing settings

There are two layers:

1. **Build defaults:** `idf.py menuconfig` → *WT32 PLC Bridge*, or `sdkconfig.defaults`. `sdkconfig.defaults` is only applied when no `sdkconfig` exists, so after editing it, delete `sdkconfig` or run `idf.py fullclean`.
2. **Saved settings (Mode A):** the Wi-Fi network, PLC IP and setup password set on the setup page or with the console are stored on the bridge. They **override the build defaults**, survive firmware updates, and take effect after a restart. `factory-reset` (console) erases them and goes back to the build defaults.

## Build & flash

```bash
idf.py build
```

```bash
idf.py -p COM6 flash monitor
```

If your adapter can't put the board into bootloader mode automatically:

1. Wire `TX0` → adapter RX, `RX0` → adapter TX, and GND ↔ GND. Power the board from its own 5 V supply.
2. Hold `IO0` at GND and power-cycle (or pulse `EN`) to enter the bootloader, then flash.
3. Release `IO0` from GND and reset to run.

## Mode A: PLC on your existing Wi-Fi

### 1. Configure

In `sdkconfig.defaults` (or menuconfig):

| Option | Set to |
| :-- | :-- |
| `CONFIG_BRIDGE_STA_SSID` / `CONFIG_BRIDGE_STA_PASSWORD` | Your existing **2.4 GHz** network. The ESP32 can't use 5 GHz |
| `CONFIG_BRIDGE_PLC_IP` | The PLC's static IP (recommended), e.g. `192.168.8.50`. Empty = learn it from the PLC's traffic |
| `CONFIG_BRIDGE_FORWARD_DCP` | `y` (default): lets TIA Portal's "Accessible devices" scan try to reach the PLC over Wi-Fi |

### 2. Move the PLC into the network's subnet

In Mode A the PLC is a client on your network, so it needs an address there: unused, outside the router's DHCP pool. For example, a GL.iNet router defaults to `192.168.8.1/24` and hands out `.100`–`.249`, so `192.168.8.50` works:

- **IP address:** `192.168.8.50`, **mask** `255.255.255.0`
- **Router (gateway):** `192.168.8.1`. Only needed if the PLC must reach other subnets.

Set it in TIA Portal (device configuration → PROFINET interface → IP protocol) and download. Doing this through the Mode 0 bridge or a direct cable is fine. **The CPU must be in STOP for the new address to be applied**; check that it really changed before moving on. Then set `CONFIG_BRIDGE_PLC_IP` to the same address.

### 3. Check the router

- The SSID is available on **2.4 GHz** (not 5 GHz only).
- **Client isolation / AP isolation** is off, otherwise Wi-Fi clients can't reach each other (laptop ↔ PLC).
- After the bridge connects, the router's client list shows the **PLC's MAC address** (Siemens), not the ESP32's.

### 4. Expected log

```text
bridge: PLC IP 192.168.8.50
bridge: No cached PLC MAC - waiting for the PLC on Ethernet
bridge: PLC found: 28:63:36:xx:xx:xx at 192.168.8.50 - cloning its MAC onto the Wi-Fi station
bridge: Wi-Fi station started as 28:63:36:xx:xx:xx, joining "<your SSID>"
bridge: Wi-Fi connected to <AP MAC> on channel 6
bridge: Gratuitous ARP sent for 192.168.8.50
bridge: eth up, wifi up (rssi -52) | eth->wifi 120 (queue full 0, tx fail 0) | wifi->eth 340 (tx fail 0) | reconnects 0
```

The PLC MAC is cached in NVS, so on later boots Wi-Fi starts straight away (`Using cached PLC MAC …`) and the PLC is re-confirmed once Ethernet is up. If a different PLC is connected, its MAC is detected and the station restarts with the new one.

### 5. Test (plan tests 3–9, 10, 12–16)

The laptop stays on its normal Wi-Fi with automatic IP (undo any static IP from Mode 0 with `netsh interface ipv4 set address name="WiFi" dhcp`).

1. `arp -d *` (admin), then `ping 192.168.8.50`, then `arp -a`: the PLC's IP maps to the **PLC's own MAC**.
2. `Test-NetConnection 192.168.8.50 -Port 102` → `TcpTestSucceeded : True`.
3. TIA Portal: *Online access* → Wi-Fi adapter. Try *Update accessible devices* (DCP; works only if the router passes it). Otherwise type `192.168.8.50` as the target address. Go online, monitor, download.
4. Robustness: `ping -n 1000 192.168.8.50` (no loss), reboot the router (the bridge reconnects; ping resumes), power-cycle the PLC, power-cycle the bridge.

## Mode A: setup page, console and firmware updates

### First flash with Phase 4

The partition table changed (two app slots), so the first Phase 4 flash must go over USB: `idf.py -p COM6 flash`. The settings area (NVS) keeps its place, so the cached PLC MAC survives and the bridge reconnects straight away. Later updates can go over Wi-Fi.

Change `CONFIG_BRIDGE_SETUP_AP_PASSWORD` (placeholder `ChangeMe-Setup`) before flashing, or right afterwards on the setup page.

### Setup page

The bridge opens a separate WPA2 network, **`WT32-Bridge-Setup`**, and serves **http://192.168.4.1** on it:

- **Status:** Ethernet link, Wi-Fi (network, signal, channel), PLC MAC and IP, frame counters and drops, reconnects, uptime, last reset reason, free memory.
- **Settings:** Wi-Fi network and password, PLC IP, setup password. *Save and restart* applies them in about 5 s. Leaving a password field empty keeps the saved one.
- **Firmware update:** upload `build/wt32_eth2ap.bin`. Images from other projects are refused.

The setup network has its own MAC address and never carries PLC traffic. It opens:

- for 10 minutes after **every power-up or restart** (power-cycling the bridge is the "button"),
- when the bridge has had **no Wi-Fi for 2 minutes**,
- whenever **no Wi-Fi network is configured**,
- on the console command `setup on`.

It closes after 10 idle minutes, but only while the bridge is connected to your Wi-Fi.

### Serial console

Type commands in the serial monitor (`idf.py -p COM6 monitor`) at the `bridge>` prompt. Log lines may interleave with it.

| Command | Does |
| :-- | :-- |
| `status` | Links, PLC, counters, setup access point |
| `wifi <ssid> [password]` | Saves the Wi-Fi network (quote names with spaces) |
| `plcip <a.b.c.d \| auto>` | Saves the PLC IP |
| `setup on` / `setup off` | Opens or closes the setup access point |
| `setuppass <password>` | Saves the setup access point password |
| `restart` | Restarts and applies saved settings |
| `factory-reset` | Erases saved settings and the cached PLC MAC, then restarts |
| `help` | Lists the commands |

### Firmware updates and rollback

An uploaded image is written to the other app slot and started. It's confirmed once the bridge connects to Wi-Fi or the setup page loads. If it crashes or gets stuck before that, the next restart (at the latest the 10-minute Wi-Fi watchdog) **returns to the previous firmware** automatically. Update while you're not actively online with TIA Portal: writing flash briefly pauses the bridge.

## Mode 0: bridge's own access point

Set `CONFIG_BRIDGE_MODE_SOFTAP=y` (instead of `CONFIG_BRIDGE_MODE_STA_CLONE=y`) and the `CONFIG_EXAMPLE_WIFI_*` options.

- The **SoftAP only comes up after the Ethernet link is up**, so the PLC must be connected and powered. It shuts down again when the link drops.
- The monitor shows `Ethernet Link Up`, then `Wi-Fi AP got a station connected` when the laptop joins.
- There is **no DHCP server**: give the laptop's Wi-Fi adapter a static IP in the PLC's subnet (admin prompt; the adapter is named `WiFi` on this PC, check yours with `Get-NetAdapter`):

  ```bash
  netsh interface ipv4 set address name="WiFi" static 192.168.0.100 255.255.255.0
  ```

  Switch it back to DHCP afterwards:

  ```bash
  netsh interface ipv4 set address name="WiFi" dhcp
  ```

- TIA Portal's *Update accessible devices* lists the PLC; go online and download as usual.

## Troubleshooting

| Symptom | Likely cause |
| :-- | :-- |
| EMAC `reset timeout`, no Ethernet | The 50 MHz oscillator isn't running. Check `CONFIG_ETHERNET_EXT_OSC_EN_GPIO_NUM=16` and that *Board Specific Init* is set to *Default Init Function* |
| Board boots into download mode or boots erratically | `IO0` still tied to GND, or the supply is too weak (Wi-Fi TX peaks) |
| PHY not detected | PHY address. Try `CONFIG_ETHERNET_PHY_ADDR=-1` (auto-detect) to confirm |
| Mode A: `No Wi-Fi network configured` | `CONFIG_BRIDGE_STA_SSID` is empty |
| Mode A: waits forever at `waiting for the PLC on Ethernet` | Wi-Fi only starts once the PLC's MAC is known. `Ignoring traffic from <IP>` means the PLC has a different IP than `CONFIG_BRIDGE_PLC_IP`. `No answer from <IP>` means nothing at that address replied. Fix the IP, or leave it empty to learn it, and unplug/replug the Ethernet cable so the PLC announces itself |
| Mode A: repeated `Wi-Fi disconnected (reason …)` | **201**: network not found (wrong SSID, or 5 GHz only). **15 / 202 / 204**: usually a wrong password. **210 / 211**: security mode mismatch. Also check MAC filtering on the router. Full list: ESP-IDF `wifi_err_reason_t` |
| Mode A: connected, but no ping | PLC IP not in the router's subnet, client isolation on, or the laptop still has a static IP |
| Mode A: `Setup access point unavailable` / `disabled` | The setup password is shorter than 8 characters. Set one with the console: `setuppass <password>`, then `restart` |
| Mode A: setup page doesn't open | Join `WT32-Bridge-Setup` first, then open `http://192.168.4.1` (not https). Phones may need "stay connected" because the network has no internet |
| Mode A: `sdkconfig.defaults` Wi-Fi/PLC IP changes ignored | Settings saved on the bridge override the build defaults. Change them on the setup page, or run `factory-reset` |
| Mode A: firmware update "not WT32 PLC Bridge firmware" | Wrong file. Upload `build/wt32_eth2ap.bin`, not the bootloader, partition table or another project's image |
| Mode A: `eth->wifi … tx fail` keeps rising | Weak signal or a congested channel. Check `rssi` in the statistics line |
| Mode 0: SoftAP not visible | No Ethernet link: the PLC is off or the cable is unplugged. The AP only starts on link-up |
| Mode 0: laptop gets no IP | Expected: there's no DHCP. Use a static IP |
| `sdkconfig.defaults` edits have no effect | Delete `sdkconfig` or run `idf.py fullclean` |
| Windows build errors like "opening dependency file … No such file or directory" | Path too long. Keep the project and build directory at short paths |
