#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

static constexpr const char* PORT_ASSIGNMENTS_PATH = "/etc/journeyos/port_assignments.json";

// Mirrors CanPortAssignment from the proto — kept as a plain C++ struct so
// the rest of the code is not coupled to protobuf-c generated types.
struct PortAssignment {
    std::string port_id;
    std::string bus_name;
    std::string description;
    uint32_t    bitrate  = 0;
    uint16_t    udp_port = 0;
    bool        enabled  = true;
};

// Snapshot stored on disk after a successful DeviceAck.
struct StoredConfig {
    uint32_t                    assigned_id = 0;
    std::vector<PortAssignment> ports;
    std::string                 sha256_hex; // SHA-256 of the on-disk JSON bytes
};

// Load from PORT_ASSIGNMENTS_PATH. Returns nullopt if file is absent or malformed.
std::optional<StoredConfig> load_stored_config(
    const char* path = PORT_ASSIGNMENTS_PATH);

// Serialise cfg to JSON and write atomically (rename-into-place).
// Computes and stores sha256_hex into cfg before writing.
bool save_stored_config(
    StoredConfig& cfg,
    const char* path = PORT_ASSIGNMENTS_PATH);

// SHA-256 of arbitrary in-memory data.
std::string sha256_hex_of(const std::string& data);

// SHA-256 of an on-disk file. Returns "" if the file cannot be read.
std::string sha256_hex_of_file(const char* path);
