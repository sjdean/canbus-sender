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

## Architecture

### Lifecycle (`src/bridge.cpp` — `run_bridge`)

The outer loop in `run_bridge` drives the entire lifetime:

1. Load `AppConfig` from `/etc/journeyos/canbus_signals.json` (`config.cpp`)
2. Parse the binary dhcpcd lease file to find the HU IP + management port (`dhcp.cpp`)
3. TCP connect → `DeviceAnnounce` → `DeviceAck` (`do_register`)
   - `UPDATE_REQUIRED`: read following `ConfigPush`, write new config atomically, restart loop
   - `REJECTED`: exit 1
4. Open one `AF_CAN / SOCK_RAW` socket and one pre-connected UDP socket per bus
5. Enter `run_poll_loop` — single-threaded `poll()` covering all CAN fds + TCP fd
6. On TCP drop: 10 s delay, restart from step 1

### poll() loop (`run_poll_loop`)

`fds[0]` = TCP management socket, `fds[1..n]` = CAN sockets.  
Timeout is computed to fire exactly at the next 30 s heartbeat deadline.  
Heartbeat is checked immediately after every `poll()` return (whether due to timeout or events), before processing any fd events.

### UDP datagram format

Each CAN frame → 20-byte ASCII: `CCCCAABBCCDDEEFFGGHH`  
`CCCC` = lower 16 bits of `can_id` as uppercase hex, `AA..HH` = 8 data bytes zero-padded if DLC < 8.

### TCP wire framing (`src/proto_io.cpp`)

4-byte big-endian length prefix + serialised `Envelope` protobuf. All send/recv goes through `send_envelope` / `recv_envelope`. `EnvelopePtr` is a `unique_ptr` with a custom deleter that calls `journeyos__device__envelope__free_unpacked`.

### Config loading (`src/config.cpp`)

`load_config(iface_map)` reads the JSON, runs a hand-written recursive-descent parser (no external JSON library), merges the bus→port mapping from the file with the bus→iface mapping supplied at runtime via `--map`, and computes the SHA-256 of the raw file bytes via OpenSSL EVP.  
`write_config` writes atomically via a `.tmp` rename.

### DHCP discovery (`src/dhcp.cpp`)

Reads the raw binary DHCP packet from `/var/lib/dhcpcd/<iface>.lease` (falls back to `/var/lib/dhcpcd5/`). Parses DHCP options TLV: option 54 for server IP, option 43 for `"journeyos:<port>"`.

### protobuf-c naming conventions

Package `journeyos.device` → C prefix `Journeyos__Device__`.  
Init macros: `JOURNEYOS__DEVICE__ENVELOPE__INIT`, etc.  
Oneof case constants: `JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_ANNOUNCE`, `…_ACK`, `…_HEARTBEAT`, `…_CONFIG_PUSH`.  
Ack status: `JOURNEYOS__DEVICE__DEVICE_ACK__STATUS__OK / REJECTED / UPDATE_REQUIRED`.

## Runtime invocation

```bash
canbus-sender --iface eth0 --map media=can0 --map diagnostic=can1
```

`--iface` is the network interface whose DHCP lease carries Option 43.  
`--map` binds a logical bus name (key in the JSON `buses` object) to a Linux CAN interface.

## dnsmasq Option 43 (HU side)

```
dhcp-option=43,"journeyos:5555"
```
