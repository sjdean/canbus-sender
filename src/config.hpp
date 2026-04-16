#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

static constexpr const char* CONFIG_PATH = "/etc/journeyos/canbus_signals.json";

struct BusConfig {
    std::string iface;  // CAN interface name (e.g. "can0")
    uint16_t    port;   // UDP destination port on the HU
};

struct AppConfig {
    std::map<std::string, BusConfig> buses;  // bus_name → {iface, port}
    std::string sha256_hex;                  // SHA-256 hex of the raw config file
};

// Load /etc/journeyos/canbus_signals.json and merge with the runtime
// interface map (bus_name → CAN iface, e.g. {"media":"can0"}).
std::optional<AppConfig> load_config(
    const std::map<std::string, std::string>& iface_map,
    const char* path = CONFIG_PATH);

// Write raw bytes to path atomically (rename-into-place).
bool write_config(const char* path, const uint8_t* data, size_t len);