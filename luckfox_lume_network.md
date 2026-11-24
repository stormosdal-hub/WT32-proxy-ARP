# Luckfox Lume: network setup behind the WT32 Wi-Fi bridge

*Written 2026-11-24, from what was read on the board over `adb shell` that day and tested from the laptop over Wi-Fi.*

The Luckfox Lume has no Wi-Fi. It's on the switch by Ethernet, and the WT32-ETH01 bridge puts it on the GL.iNet Wi-Fi network using the bridge's *Also bridge other devices on the switch* feature (README section 6). It now has a fixed address, **`192.168.0.60`**, and accepts SSH logins as `root` over Wi-Fi.

## The board

| | |
| :-- | :-- |
| OS | Buildroot 2022.05 (`aiot-t153-linux-v1.0-1-g152b6122-dirty`) |
| Kernel | Linux 5.10.198, armv7l |
| Hostname | `luckfox` |
| Root filesystem | UBIFS, read-write, so changes survive reboots |
| Network manager | `dhcpcd` 9.4.1, started by `/etc/init.d/S41dhcpcd` |
| SSH server | OpenSSH 9.0 (`sshd`), started by `/etc/init.d/S50sshd` |
| `eth0` | MAC `02:00:9f:dd:69:54`, cabled to the switch |
| `eth1` | MAC `12:00:9f:dd:69:54`, not connected |
| Users | `root` is the only login user |

## Network

| | |
| :-- | :-- |
| Address | `192.168.0.60/24` on `eth0`, static |
| Gateway | `192.168.0.254` (GL.iNet GL-MT3000) |
| DNS | `192.168.0.254` |
| Path to Wi-Fi | `eth0` → switch → WT32-ETH01 (Mode A, *bridge other devices*) → Wi-Fi `GL-MT3000-091` |

**Why the address shows the PLC's MAC elsewhere:** the access point only knows the bridge under the PLC's MAC (`30:b8:51:c1:29:1e`). So the bridge sends the Luckfox's frames under that MAC and gives them back the Luckfox's own MAC on the way in. On the laptop (`arp -a`) and in the router's client list, `192.168.0.60` therefore appears with the PLC's MAC. That's expected.

**Why static:** DHCP replies can't get through the bridge. Before the change, `dhcpcd` fell back to the automatic link-local address `169.254.17.59`.

## Changes made

### 1. Static address in `/etc/dhcpcd.conf`

- **Backup first:** `/etc/dhcpcd.conf.bak-20261124`.
- **Appended this block to the end of the file:**

```text
# Static address for the WT32 Wi-Fi bridge (DHCP cannot get through it)
interface eth0
static ip_address=192.168.0.60/24
static routers=192.168.0.254
static domain_name_servers=192.168.0.254
```

- **Applied without a reboot** with `dhcpcd -n eth0`, which makes `dhcpcd` re-read its config for `eth0`. The link-local address went away. A temporary address and route added by hand for testing beforehand were replaced by `dhcpcd`'s own.

### 2. Root password

`root` had **no password**. `adb shell` logs in without one, but SSH refuses accounts with an empty password (`PermitEmptyPasswords no` by default), so `ssh` answered "Permission denied". You set a password yourself with `passwd` in `adb shell`. It's not written down here.

### Not changed

- **`/etc/ssh/sshd_config`:** untouched. It has `PermitRootLogin yes`, and everything else is at OpenSSH defaults, including password logins.
- **SSH keys:** no `/root/.ssh/authorized_keys`.
- **`eth1`:** not configured. If it's ever cabled, `dhcpcd` will try DHCP on it.

## Access

| From | Command |
| :-- | :-- |
| PC over USB | `adb shell` |
| Anywhere on the Wi-Fi (or the switch) | `ssh root@192.168.0.60` |

## Verification (2026-11-24)

**From the laptop over Wi-Fi only** (its Ethernet cable unplugged, so the traffic really went through the bridge):

| Test | Result |
| :-- | :-- |
| Ping | 0 % loss, about 8–12 ms average |
| Full-size packets (1500 bytes, don't fragment) | Pass |
| SSH | Port 22 answers `SSH-2.0-OpenSSH_9.0`; login as `root` works with the new password |

**From the Luckfox:**

| Test | Result |
| :-- | :-- |
| Ping the router `192.168.0.254` | 0 % loss, 4.5 ms |
| DNS + internet | `example.com` resolves via `192.168.0.254` and answers, 43 ms |

**Same time, the PLC (`10.0.123.52`):** still 0 % loss through the bridge.

**Not verified:** a reboot. The config is on the read-write root filesystem and `S41dhcpcd` starts `dhcpcd` with it at boot, so it should come back as `192.168.0.60`. Rebooting once and running `ip -4 addr show eth0` confirms it.

## Gotchas

- **`/etc/init.d/S41dhcpcd restart` doesn't restart `dhcpcd` on this board.**
  - **Why:** the script's `stop` looks for `/var/run/dhcpcd.pid`, which `dhcpcd` 9.4.1 doesn't use. So nothing is stopped, and `start` only passes a message to the already-running `dhcpcd`, which keeps its old config.
  - **What to do:** after editing `/etc/dhcpcd.conf`, run `dhcpcd -n eth0`, or reboot.
- **DHCP doesn't work through the bridge.** Keep the static address; to move the board, change the block above.
- **IPv4 only:** the bridge doesn't carry the Luckfox's IPv6 traffic.
- **Testing from the laptop:** with the laptop's Ethernet cable still in the switch, Windows may reach the Luckfox over the cable instead of Wi-Fi.

## Security

- **SSH as `root` is open to everything on the Wi-Fi,** with password login allowed. Keep the password strong.
- **Stricter, optional:** put your PC's public key in `/root/.ssh/authorized_keys`, check that key login works, then set `PasswordAuthentication no` in `/etc/ssh/sshd_config` and restart `sshd`.
- **This file holds no passwords.**

## Undo

Back to DHCP (behind the bridge that means the link-local fallback again):

```bash
cp /etc/dhcpcd.conf.bak-20261124 /etc/dhcpcd.conf
```

```bash
dhcpcd -n eth0
```
