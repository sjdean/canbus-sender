# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`canbus-sender` — a Linux/Raspberry Pi daemon that bridges SocketCAN interfaces to UDP and registers with a JourneyOS head unit (HU) over a ProtoBuf/TCP management protocol.

Target platform: Raspberry Pi OS (ARM). Linux-specific headers (`linux/can.h`, `linux/can/raw.h`, `poll.h`, `/sys/class/net/`) are used throughout; the code will not compile on macOS.

## Build

```bash
# Install deps on the Pi
sudo apt install libprotobuf-c-dev protobuf-c-compiler libssl-dev cmake build-essential

# Configure + build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Optional: embed firmware version
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFIRMWARE_VERSION=2.1.0

# Install to /usr/local/bin
sudo cmake --install build
```

`protoc` runs at build time to generate `build/proto_gen/journeyos_device.pb-c.{c,h}` from `proto/journeyos_device.proto`. These generated files are an include path for all source files.

Debug build: `cmake -B build -DCMAKE_BUILD_TYPE=Debug`

## Runtime invocation

```bash
canbus-sender --iface eth0
```

`--iface` is the network interface whose DHCP lease carries Option 43 (`journeyos:<port>`). Port assignments (bus name, CAN interface, bitrate, UDP port) are received from the HU on first boot and stored in `/etc/journeyos/port_assignments.json`. No `--map` arguments are needed.

## Architecture

### Lifecycle (`src/bridge.cpp` — `run_bridge`)

The outer loop drives the entire lifetime:

1. `load_stored_config()` — loads `/etc/journeyos/port_assignments.json` (absent = virgin device)
2. `enumerate_can_ports()` (`can_probe.cpp`) — scans `/sys/class/net` for `ARPHRD_CAN=280` interfaces, reads bitrate/FD/operstate from sysfs
3. `discover_hu()` (`dhcp.cpp`) — parses binary dhcpcd lease for option 54 (HU IP) + option 43 (`journeyos:<port>`)
4. `tcp_connect_nb()` — non-blocking connect with `CONNECT_TIMEOUT_MS = 5000` timeout
5. If HU **reachable**: full registration → `DeviceAck` with `CanPortAssignment` list → store + apply
6. If HU **unreachable** + stored config exists: enter degraded mode (CAN forwarding with 60 s reconnect timer)
7. If HU **unreachable** + no stored config: wait 10 s and retry (virgin device cannot operate alone)

### Virgin vs. known-config device

| State | Behaviour |
|---|---|
| No `/etc/journeyos/port_assignments.json` | Loops until HU responds and provides `DeviceAck.can_ports` |
| File exists, HU online | Registers; updates stored config if `DeviceAck` differs |
| File exists, HU offline | Forwards with stored config; retries HU every 60 s in poll loop |

`config_hash` in `DeviceAnnounce` = SHA-256 of stored JSON file (empty string on virgin).

### poll() loop (`run_poll_loop`)

Unified loop covering both registered and degraded modes, distinguished by `tcp_fd`:
- `tcp_fd >= 0` (registered): `fds[0]` = TCP, `fds[1..n]` = CAN; 30 s heartbeat timer
- `tcp_fd == -1` (degraded): `fds[0..n-1]` = CAN only; 60 s reconnect probe timer

Timer fires when `now >= last_action + interval`; checked after every `poll()` return.

### CAN port discovery (`src/can_probe.cpp`)

`enumerate_can_ports()` reads:
- `/sys/class/net/<iface>/type` == 280 to identify CAN interfaces
- `/sys/class/net/<iface>/can/bitrate` for current bitrate (0 = not configured)
- `/sys/class/net/<iface>/can/ctrlmode_supported` bit 0x20 (`CAN_CTRLMODE_FD`) for FD capability; falls back to checking for existence of `data_bitrate` sysfs entry
- `/sys/class/net/<iface>/operstate` (`"up"` or `"unknown"`) for link_active

`configure_can_port()` runs `ip link set <iface> down/type can bitrate N/up` via `system()`. Interface names are validated (alphanumeric + `-_`, ≤ 15 chars) before use.

### Stored config (`src/config.cpp`)

`StoredConfig` mirrors `DeviceAck.can_ports` as a plain C++ struct vector. Serialised as human-readable JSON to `/etc/journeyos/port_assignments.json` with atomic rename-into-place. SHA-256 (OpenSSL EVP) is computed over the serialised bytes and stored in `StoredConfig::sha256_hex`.

`save_stored_config(cfg&)` mutates `cfg.sha256_hex` before writing.

### DeviceAnnounce flow

`DeviceAnnounce` now carries two distinct things:
- `can_ports` (field 7) — live sysfs probe: what ports exist and their current state
- `config_hash` (field 6) — hash of what the device is currently *configured* to do (from stored config)

The HU uses `can_ports` to decide assignment and `config_hash` to detect stale configs.

### UDP datagram format

Each CAN frame → 20-byte ASCII: `CCCCAABBCCDDEEFFGGHH`  
`CCCC` = lower 16 bits of `can_id` as uppercase hex, `AA..HH` = 8 data bytes zero-padded if DLC < 8.

### TCP wire framing (`src/proto_io.cpp`)

4-byte big-endian length prefix + serialised `Envelope` protobuf. `EnvelopePtr` is a `unique_ptr` with custom deleter calling `journeyos__device__envelope__free_unpacked`.

### DHCP discovery (`src/dhcp.cpp`)

Reads raw binary DHCP packet from `/var/lib/dhcpcd/<iface>.lease` (falls back to `/var/lib/dhcpcd5/`). Parses DHCP options TLV: option 54 for server IP, option 43 for `"journeyos:<port>"`.

### protobuf-c naming conventions

Package `journeyos.device` → C prefix `Journeyos__Device__`.  
Init macros: `JOURNEYOS__DEVICE__ENVELOPE__INIT`, `JOURNEYOS__DEVICE__CAN_PORT_INFO__INIT`, etc.  
Oneof case constants: `JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_ANNOUNCE / _ACK / _HEARTBEAT / _CONFIG_PUSH`.  
Ack status: `JOURNEYOS__DEVICE__DEVICE_ACK__STATUS__OK / REJECTED / UPDATE_REQUIRED`.  
New repeated fields in `DeviceAnnounce`: `n_can_ports` / `can_ports`.  
New repeated fields in `DeviceAck`: `n_can_ports` / `can_ports` (type `CanPortAssignment`).

## dnsmasq Option 43 (HU side)

```
dhcp-option=43,"journeyos:5555"
```
