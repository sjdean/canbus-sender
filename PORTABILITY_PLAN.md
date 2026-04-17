# Portability Plan — canbus-sender

**Goal:** Make the codebase buildable and runnable on three platforms:

| Platform | Role | Networking | CAN |
|---|---|---|---|
| Raspberry Pi (Linux) | Development / testing reference | POSIX sockets, dhcpcd | SocketCAN (`AF_CAN`) |
| ESP32 (ESP-IDF) | Production target | lwIP BSD sockets, built-in DHCP | TWAI peripheral |
| STM32 (STM32 HAL + FreeRTOS) | Production target | lwIP via Ethernet MAC | bxCAN / FDCAN peripheral |

The core protocol logic (registration state machine, heartbeat, degraded mode, protobuf
message construction) is already well-isolated in `bridge.cpp` and contains no
platform-specific code. The work is to introduce a HAL layer that the core calls into,
and provide three platform implementations behind it.

---

## 1. Platform dependency map

Every Linux-specific call in the current code maps to one of six capability areas:

| Area | Current implementation | Files affected |
|---|---|---|
| **CAN I/O** | `AF_CAN` raw socket, `linux/can.h`, `SIOCGIFINDEX` | `bridge.cpp` |
| **CAN management** | `/sys/class/net/` sysfs reads, `system("ip link set ...")` | `can_probe.cpp` |
| **Network I/O** | POSIX `socket()`, `connect()`, `poll()`, `fcntl()` | `bridge.cpp`, `proto_io.cpp` |
| **DHCP discovery** | dhcpcd binary lease file (`/var/lib/dhcpcd/`) | `dhcp.cpp` |
| **Config storage** | POSIX filesystem, atomic `rename()` | `config.cpp` |
| **Crypto + time** | OpenSSL EVP SHA-256, `clock_gettime(CLOCK_MONOTONIC)` | `config.cpp`, `bridge.cpp` |

---

## 2. Proposed directory structure

```
canbus-sender/
├── core/                        # Platform-agnostic (no #includes of Linux headers)
│   ├── bridge.cpp / .hpp        # State machine — unchanged
│   ├── config.cpp / .hpp        # JSON parse/serialise — unchanged
│   └── proto_io.cpp / .hpp      # Protobuf framing — use platform net HAL
│
├── platform/
│   ├── hal.hpp                  # Abstract HAL interfaces (pure C++ headers)
│   ├── linux/
│   │   ├── can_hal.cpp          # SocketCAN implementation
│   │   ├── net_hal.cpp          # POSIX sockets implementation
│   │   ├── dhcp_hal.cpp         # dhcpcd lease file implementation
│   │   ├── storage_hal.cpp      # POSIX filesystem implementation
│   │   └── sys_hal.cpp          # OpenSSL SHA-256, clock_gettime, MAC from sysfs
│   ├── esp32/
│   │   ├── can_hal.cpp          # ESP-IDF TWAI driver
│   │   ├── net_hal.cpp          # lwIP BSD sockets (nearly identical to Linux)
│   │   ├── dhcp_hal.cpp         # lwIP DHCP callback / option 43 hook
│   │   ├── storage_hal.cpp      # SPIFFS / LittleFS via esp_vfs
│   │   └── sys_hal.cpp          # mbedTLS SHA-256, esp_timer, esp_wifi_get_mac
│   └── stm32/
│       ├── can_hal.cpp          # STM32 HAL bxCAN / FDCAN driver
│       ├── net_hal.cpp          # lwIP BSD sockets (thin wrapper)
│       ├── dhcp_hal.cpp         # lwIP DHCP callback / option 43 hook
│       ├── storage_hal.cpp      # Flash sector or SPI-NOR via FatFS / LittleFS
│       └── sys_hal.cpp          # mbedTLS SHA-256, HAL_GetTick or FreeRTOS ticks
│
├── proto/
│   └── journeyos_device.proto   # unchanged
│
└── CMakeLists.txt               # extended with PLATFORM selection
```

`can_probe.cpp` and `dhcp.cpp` are dissolved into their respective `platform/*/` files.
`bridge.cpp` is moved to `core/` and calls only through `hal.hpp`.

---

## 3. HAL interface definitions (`platform/hal.hpp`)

All interfaces are thin; no virtual dispatch. Each platform provides concrete
implementations that are linked at compile time. The `core/` code `#includes` only
`hal.hpp` — never any platform header.

### 3.1 Logging

```cpp
// platform/hal.hpp

namespace hal {

// Severity levels map to platform equivalents:
//   Linux  → fprintf(stderr, ...)
//   ESP32  → ESP_LOGE / ESP_LOGW / ESP_LOGI
//   STM32  → UART printf or RTT
void log(const char* tag, const char* fmt, ...) __attribute__((format(printf,2,3)));

} // namespace hal
```

Replace all `fprintf(stderr, ...)` calls in `core/` with `hal::log(TAG, ...)`.

### 3.2 Monotonic clock

```cpp
namespace hal {

// Milliseconds since boot (wraps after ~49 days on 32-bit targets; use uint64_t).
uint64_t uptime_ms();

// Block current task / thread for at least `ms` milliseconds.
void sleep_ms(uint32_t ms);

} // namespace hal
```

Replaces `clock_gettime(CLOCK_MONOTONIC)` and `sleep()` in `bridge.cpp`.

### 3.3 Crypto

```cpp
namespace hal {

// Return lowercase hex-encoded SHA-256 of `data`.
std::string sha256_hex(const std::string& data);

} // namespace hal
```

| Platform | Implementation |
|---|---|
| Linux | `EVP_DigestInit_ex` / `EVP_sha256()` (OpenSSL) |
| ESP32 | `mbedtls_sha256()` (bundled with ESP-IDF) |
| STM32 | `mbedtls_sha256()` or STM32 hardware hash (HASH peripheral on F4/H7) |

### 3.4 Device identity

```cpp
namespace hal {

// Return a stable unique device identifier string (typically MAC address).
// Format: "AA:BB:CC:DD:EE:FF"
std::string device_id();

} // namespace hal
```

| Platform | Source |
|---|---|
| Linux | `/sys/class/net/<iface>/address` |
| ESP32 | `esp_wifi_get_mac()` or `esp_eth_iodriver_get_mac()` |
| STM32 | `HAL_ETH_GetMACConfig()` or UID registers |

### 3.5 Network (TCP + UDP)

The current code uses POSIX file descriptors throughout. The simplest approach is to
keep the fd-based API — both lwIP (ESP32 and STM32) and POSIX (Linux) expose BSD
sockets returning integer fds with the same semantics.

```cpp
namespace hal::net {

// Non-blocking TCP connect with timeout. Returns fd >= 0 on success, -1 on failure.
int tcp_connect(const char* ip, uint16_t port, int timeout_ms);

// Create a connected UDP socket to dst_ip:dst_port. Returns fd >= 0 or -1.
int udp_open(const char* dst_ip, uint16_t dst_port);

// poll()-equivalent over an array of fds. Returns number of ready fds, 0=timeout, -1=error.
// events / revents use POLLIN / POLLOUT / POLLHUP constants.
int poll_fds(struct PollFd* fds, int n, int timeout_ms);

struct PollFd {
    int      fd;
    uint16_t events;
    uint16_t revents;
};

// Constants (same values as POSIX — lwIP defines them identically):
static constexpr uint16_t POLLIN  = 0x0001;
static constexpr uint16_t POLLOUT = 0x0004;
static constexpr uint16_t POLLHUP = 0x0010;
static constexpr uint16_t POLLERR = 0x0008;

void close_fd(int fd);

} // namespace hal::net
```

On Linux this wraps `poll()` / `socket()` / `close()` directly.
On ESP32 / STM32 with lwIP, lwIP's BSD socket layer provides the same calls; the
wrapper is nearly a no-op.

`proto_io.cpp` already uses `read()`/`write()` on fds — those map 1:1 to lwIP's
`recv()`/`send()` when building with lwIP's POSIX compat shim enabled
(`CONFIG_LWIP_POSIX_SOCKETS_IO_NAMES=y` in ESP-IDF, or equivalent in STM32's lwIP
`lwipopts.h`).

### 3.6 DHCP discovery

```cpp
namespace hal {

// Block until the HU IP and management port are known.
// On Linux:   parse dhcpcd lease file.
// On ESP32:   register a lwIP DHCP callback that captures option 43.
// On STM32:   same lwIP callback approach.
//
// Returns nullopt if no valid lease found within a reasonable timeout.
std::optional<HUInfo> discover_hu();

} // namespace hal
```

The `iface` string parameter present in the current Linux implementation is removed
from the HAL signature — it is Linux-specific. Each platform knows its own interface
internally (the primary_iface concept moves into the platform init, not the core).

### 3.7 CAN enumeration and management

```cpp
namespace hal::can {

struct PortInfo {
    std::string port_id;      // "can0", "twai0", "fdcan1", etc.
    uint32_t    bitrate;      // 0 = not yet configured
    bool        fd_capable;
    bool        link_active;
};

// Return all available CAN ports, sorted by port_id.
std::vector<PortInfo> enumerate();

// Configure bitrate and bring port up. Returns false on failure.
bool configure(const std::string& port_id, uint32_t bitrate);

// Bring port administratively down.
void disable(const std::string& port_id);

} // namespace hal::can
```

| Platform | Implementation |
|---|---|
| Linux | Existing sysfs reads + `system("ip link set ...")` — moved to `platform/linux/can_hal.cpp` |
| ESP32 | `twai_driver_install()` / `twai_start()` / `twai_get_status_info()`; single port "twai0" |
| STM32 | `HAL_CAN_Init()` / `HAL_CAN_Start()` / `HAL_CAN_GetState()`; enumerate via known peripheral list |

### 3.8 CAN I/O

```cpp
namespace hal::can {

struct Frame {
    uint32_t id;        // CAN ID (11 or 29 bit)
    uint8_t  dlc;
    uint8_t  data[8];
};

// Open a receive handle for port_id. Returns opaque handle >= 0, or -1 on failure.
// On Linux this is an AF_CAN raw socket fd — poll_fds() works on it directly.
// On ESP32/STM32 this is a logical index into an internal RX queue.
int open_rx(const std::string& port_id);

// Read one frame. Blocks until a frame is available or error.
// Returns false on error / port closed.
bool read_frame(int handle, Frame& out);

// On Linux: close() the fd. On MCU: unregister the queue.
void close_rx(int handle);

} // namespace hal::can
```

**Note on poll() integration for MCUs:** On Linux, `open_rx()` returns a real fd and
`poll_fds()` works natively. On ESP32/STM32, CAN RX goes through a driver FIFO, not a
file descriptor. Two options:

- **Option A (simpler):** The poll loop on MCUs uses a FreeRTOS task-notification or
  event-group mechanism; `poll_fds()` on MCU checks lwIP sockets with `select()` and
  CAN handles with `ulTaskNotifyTakeIndexed()` in a combined wait. The HAL
  implementation handles this internally.

- **Option B (cleaner):** Create a lightweight "unified fd" abstraction where CAN
  handles are mapped into a select-like wait using a pipe/socket pair that is written
  to by the CAN RX ISR/callback. The core code remains unchanged.

Option A is recommended for the first port; Option B if the poll loop grows in
complexity.

### 3.9 Config storage

```cpp
namespace hal::storage {

// Read the full content of a named persistent resource.
// Returns nullopt if not found.
std::optional<std::string> read(const char* key);

// Write atomically (i.e. a crash mid-write must not corrupt the previous value).
// On Linux: write to <key>.tmp, then rename.
// On ESP32: NVS or LittleFS with two-slot journalling.
// On STM32: flash sector with generation counter or LittleFS.
bool write(const char* key, const std::string& data);

} // namespace hal::storage
```

`PORT_ASSIGNMENTS_PATH` in `config.hpp` becomes a key string constant
(`"port_assignments"`) rather than a filesystem path. The `config.cpp` serialiser
and parser are unchanged — they work on `std::string` in memory.

---

## 4. Changes to core files

| File | Changes required |
|---|---|
| `core/bridge.cpp` | Replace `read_mac(iface)` with `hal::device_id()`; replace `sleep()` with `hal::sleep_ms()`; replace `uptime_ms()` with `hal::uptime_ms()`; replace `poll()`/`struct pollfd` with `hal::net::poll_fds()`/`hal::net::PollFd`; replace `open_can_socket()`/`close()` with `hal::can::open_rx()`/`hal::can::close_rx()`; replace `read()` on CAN fd with `hal::can::read_frame()`; replace direct `configure_can_port()`/`disable_can_port()` calls with `hal::can::configure()`/`hal::can::disable()`; replace `enumerate_can_ports()` with `hal::can::enumerate()`; replace `discover_hu(iface)` with `hal::discover_hu()` |
| `core/config.cpp` | Replace `fopen`/`rename` with `hal::storage::read()`/`hal::storage::write()`; replace OpenSSL `EVP_*` with `hal::sha256_hex()` |
| `core/proto_io.cpp` | Replace bare `read(fd)`/`write(fd)` with `recv(fd)`/`send(fd)` — these are available on both POSIX and lwIP; or wrap in `hal::net::recv_exact()`/`hal::net::send_exact()` if needed |
| `core/bridge.hpp` | Remove `primary_iface` parameter from `run_bridge()` — platform init handles this |

---

## 5. Platform-specific concerns

### 5.1 Linux (Raspberry Pi)

Minimal change. All current behaviour is preserved; code is reorganised into
`platform/linux/`. The `system("ip link set ...")` approach in `can_hal.cpp` is kept
as-is — it is correct for Linux but must not leak into the HAL interface.

### 5.2 ESP32

- **Entry point:** `app_main()` initialises NVS, Ethernet/WiFi, waits for IP, then
  calls `run_bridge()` from a FreeRTOS task.
- **TWAI:** Single CAN peripheral. `hal::can::enumerate()` returns one entry
  (`"twai0"`). Bitrate is set via `twai_driver_install()` with a new config;
  re-initialisation requires `twai_driver_uninstall()` first.
- **DHCP option 43:** Register a callback with `esp_event_handler_register()` for
  `IP_EVENT_ETH_GOT_IP` / `IP_EVENT_STA_GOT_IP`; read option 43 from lwIP's
  `netif->dhcp->offer_options`. This is set before `run_bridge()` is called and the
  result stored; `hal::discover_hu()` returns the cached value.
- **Storage:** Use `esp_vfs_littlefs_register()` and standard `fopen`/`rename` within
  the VFS mount, **or** use NVS for the config blob. LittleFS is preferred since the
  value is a multi-KB JSON string.
- **C++ STL:** Fully supported in ESP-IDF ≥ 5.x. Exceptions should be disabled
  (`-fno-exceptions`) and the config parser's `try/catch` replaced with return-code
  checks.
- **protobuf-c:** Cross-compiles cleanly for Xtensa with the ESP-IDF CMake toolchain.
  Add as a CMake `EXTRA_COMPONENT_DIRS` entry.

### 5.3 STM32

- **Entry point:** `main()` initialises clocks, HAL, Ethernet, lwIP, FreeRTOS, then
  creates a task calling `run_bridge()`.
- **CAN:** `bxCAN` (F4/F7) or `FDCAN` (H7/G4). `hal::can::enumerate()` returns a
  static list of peripherals compiled in (e.g. `{"fdcan1", "fdcan2"}`). Bitrate
  reconfiguration requires `HAL_CAN_Stop()` / reinit / `HAL_CAN_Start()`.
- **DHCP option 43:** Hook into lwIP's `dhcp_option_callback` (available from lwIP
  2.1+) or parse raw DHCP frames via a raw PCB. This is the most integration-heavy
  part of the STM32 port.
- **Storage:** FatFS on an SD card or SPI-NOR flash, or LittleFS on internal flash.
  Atomic write via a two-file scheme (write to `.tmp`, then rename) is supported by
  FatFS and LittleFS. Minimum flash allocation for config + LittleFS overhead is ~64 KB.
- **RAM:** An STM32F4 (192 KB SRAM) is tight. Key consumers:
  - lwIP: ~30–50 KB depending on configuration
  - FreeRTOS + task stacks: ~10–20 KB
  - protobuf-c serialisation buffers: ~4–8 KB
  - `std::string`/`std::vector` for port list + config: ~4–8 KB
  - STM32H7 (1 MB SRAM) is the comfortable target; F4 requires careful lwIP tuning.
- **C++ STL on bare metal:** `std::string`, `std::vector`, and `std::optional` work
  with arm-none-eabi-g++ and newlib. Disable exceptions and RTTI
  (`-fno-exceptions -fno-rtti`) and provide a `__cxa_pure_virtual` stub.
  Replace the `try/catch` in `config.cpp`'s JSON parser with error-code returns.
- **protobuf-c:** Cross-compiles cleanly for ARM Cortex-M with appropriate newlib
  stubs for `malloc`/`free` (already required by lwIP anyway).

---

## 6. Build system

Extend `CMakeLists.txt` with a `PLATFORM` option:

```cmake
set(PLATFORM "linux" CACHE STRING "Target platform: linux | esp32 | stm32")
```

Each platform provides its own source list and toolchain:

```cmake
if(PLATFORM STREQUAL "linux")
    # Use existing host compiler
    set(PLATFORM_SOURCES
        platform/linux/can_hal.cpp
        platform/linux/net_hal.cpp
        platform/linux/dhcp_hal.cpp
        platform/linux/storage_hal.cpp
        platform/linux/sys_hal.cpp)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(... OpenSSL::Crypto)

elseif(PLATFORM STREQUAL "esp32")
    # Use idf.py / ESP-IDF CMake integration
    # ESP-IDF component wraps this CMakeLists.txt
    set(PLATFORM_SOURCES
        platform/esp32/can_hal.cpp
        ...

elseif(PLATFORM STREQUAL "stm32")
    # CMAKE_TOOLCHAIN_FILE=arm-none-eabi.cmake must be provided
    set(PLATFORM_SOURCES
        platform/stm32/can_hal.cpp
        ...
endif()

add_executable(canbus-sender
    core/bridge.cpp
    core/config.cpp
    core/proto_io.cpp
    ${PLATFORM_SOURCES}
    ${PROTO_C_SRC})
```

For ESP32, the project becomes an ESP-IDF component (a `CMakeLists.txt` in the
component style that calls `idf_component_register()`). For STM32, a
`arm-none-eabi.cmake` toolchain file and STM32CubeMX-generated init code are required.

---

## 7. Migration phases

### Phase 1 — Introduce HAL, keep Linux working (no regression)

1. Create `platform/hal.hpp` with all interface definitions.
2. Move `can_probe.cpp` → `platform/linux/can_hal.cpp`, wrapping existing sysfs code.
3. Move `dhcp.cpp` → `platform/linux/dhcp_hal.cpp`, unchanged.
4. Extract OpenSSL SHA-256 from `config.cpp` → `platform/linux/sys_hal.cpp`.
5. Replace all direct calls in `bridge.cpp` and `config.cpp` with `hal::` calls.
6. Verify Linux build and existing behaviour is unchanged.

### Phase 2 — ESP32 port

1. Implement all five `platform/esp32/*.cpp` files.
2. Integrate protobuf-c as an ESP-IDF component.
3. Validate on hardware: CAN loopback test, DHCP discovery, registration with a
   simulated HU on the same network.

### Phase 3 — STM32 port

1. Select target MCU and resolve RAM budget with a worst-case stack/heap analysis.
2. Implement all five `platform/stm32/*.cpp` files.
3. Integrate protobuf-c, lwIP, and FatFS/LittleFS.
4. Validate on hardware: same integration tests as Phase 2.

---

## 8. What does NOT need to change

- `proto/journeyos_device.proto` — unchanged
- The protobuf-c generated files — same `protoc-gen-c` pipeline on all platforms
- `core/config.cpp` JSON serialiser/parser — pure `std::string` logic, no I/O
- `core/bridge.cpp` state machine logic — registration, degraded mode, reconnect
- The UDP datagram format (20-byte ASCII)
- The TCP framing format (4-byte big-endian length prefix)
- `HUInfo`, `StoredConfig`, `PortAssignment` structs

---

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| DHCP option 43 not available in lwIP's public API on the target lwIP version | Use a raw PCB to intercept DHCP OFFER/ACK packets directly; parse option 43 from the UDP payload — the same parsing logic as the current `dhcp.cpp` |
| STM32F4 RAM too tight for lwIP + protobuf-c + STL | Target STM32H7 as primary MCU; keep STM32F4 as a stretch goal with stripped lwIP config |
| `std::optional` / `std::string` not available with arm-none-eabi newlib | Use newlib-nano with C++17; it includes `<optional>` and `<string>` from GCC 7+. If unavailable, replace `std::optional` with a simple `struct { T value; bool valid; }` wrapper |
| CAN port re-initialisation (bitrate change) requires driver teardown on MCUs | HAL `configure()` implementations must handle stop/deinit/reinit/start internally; the core calls `configure()` exactly once after receiving `DeviceAck` so re-entry during forwarding is not a current concern |
| protobuf-c `malloc` usage on MCU heap | protobuf-c uses `malloc`/`free` for unpack; size incoming messages (DeviceAck with a handful of CanPortAssignment entries) are small — well within a 16 KB heap budget. Monitor with heap watermark checks during integration testing |