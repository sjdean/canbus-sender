# canbus-sender

A CAN-to-UDP bridge for Raspberry Pi.  
Reads frames from one or more Linux SocketCAN interfaces and forwards each frame
as a compact 20-byte ASCII UDP datagram to a JourneyOS head unit (HU).

Implements a ProtoBuf management protocol over TCP for self-registration,
config delivery, and periodic heartbeats.

---

## Build

### Prerequisites (Raspberry Pi OS / Debian)

```bash
sudo apt install \
    cmake build-essential \
    libprotobuf-c-dev protobuf-c-compiler \
    libssl-dev
```

### Compile

```bash
cd /path/to/canbus-sender
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

To embed a specific firmware version string:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFIRMWARE_VERSION=2.1.0
```

The single resulting binary is `build/canbus-sender`.

---

## Mapping bus names to CAN interfaces

The config file (`/etc/journeyos/canbus_signals.json`) names buses logically
(e.g. `"media"`, `"diagnostic"`).  The mapping from those names to Linux
SocketCAN interfaces (e.g. `can0`, `can1`) is provided at runtime via
`--map` arguments.

```
canbus-sender --iface <primary-net-iface> --map <bus-name>=<can-iface> [--map ...]
```

| Argument | Description |
|---|---|
| `--iface eth0` | Network interface whose DHCP lease contains Option 43. |
| `--map media=can0` | Map the `"media"` bus from the config to Linux CAN interface `can0`. |
| `--map diagnostic=can1` | Map the `"diagnostic"` bus to `can1`. |

### Example

```bash
canbus-sender --iface eth0 --map media=can0 --map diagnostic=can1
```

Bus names not covered by a `--map` argument are silently skipped.

---

## Config file format

`/etc/journeyos/canbus_signals.json`

```json
{
  "buses": {
    "media":      8887,
    "diagnostic": 8888
  }
}
```

Each key is a logical bus name; the value is the **UDP destination port** on the HU.

---

## UDP datagram format

Each CAN frame is encoded as a **20-byte ASCII string** (no terminator):

```
CCCCAABBCCDDEEFFGGHH
```

| Field | Width | Description |
|---|---|---|
| `CCCC` | 4 chars | Lower 16 bits of the CAN frame ID, uppercase hex (e.g. `05D7`) |
| `AA`…`HH` | 2 chars each × 8 | Data bytes, uppercase hex; DLC < 8 is zero-padded |

---

## Management protocol

On boot the bridge:

1. Parses the dhcpcd binary lease file for `--iface` to find the HU IP (DHCP Server
   Identifier, option 54) and management port (Vendor-Specific option 43:
   `journeyos:<port>`).
2. Connects TCP to `HU_IP:PORT`.
3. Sends `DeviceAnnounce` (MAC address, firmware version, UDP ports, config SHA-256).
4. Reads `DeviceAck`:
   - `OK` → starts CAN forwarding.
   - `UPDATE_REQUIRED` → reads the following `ConfigPush` (inline `config_data`),
     writes the new config to disk, and re-registers from step 1.
   - `REJECTED` → logs and exits.
5. Sends a `Heartbeat` every 30 seconds on the same TCP connection.
6. On TCP disconnect, waits 10 seconds and re-registers from step 1.

All TCP messages are framed with a 4-byte big-endian length prefix.

---

## Configuring DHCP Option 43 on dnsmasq (HU side)

Add to `/etc/dnsmasq.conf` on the head unit:

```
# Vendor-Specific (option 43) — ASCII string "journeyos:<port>"
# dhcp-option=vendor:,43,"journeyos:5555"
dhcp-option=43,"journeyos:5555"
```

Replace `5555` with the actual TCP management port your HU listens on.

Restart dnsmasq after editing:

```bash
sudo systemctl restart dnsmasq
```

The Raspberry Pi must request a fresh lease (or reboot) for the new option to
take effect.  You can verify the option was received:

```bash
# On the RPi — the lease file should contain the option 43 bytes:
strings /var/lib/dhcpcd/eth0.lease | grep journeyos
```

---

## systemd service example

`/etc/systemd/system/canbus-sender.service`

```ini
[Unit]
Description=CAN Bus UDP Bridge
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/canbus-sender --iface eth0 --map media=can0 --map diagnostic=can1
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now canbus-sender
```