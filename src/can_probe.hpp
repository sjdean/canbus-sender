#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Physical state of a CAN port as probed from the Linux kernel at startup.
struct CanPortProbe {
    std::string port_id;
    uint32_t    bitrate     = 0;     // 0 = interface not yet configured
    bool        fd_capable  = false;
    bool        link_active = false;
};

// Enumerate all CAN interfaces present in /sys/class/net (ARPHRD_CAN = 280).
// Results are sorted by port_id for deterministic ordering.
std::vector<CanPortProbe> enumerate_can_ports();

// Bring `port_id` down, configure bitrate, bring it back up.
// Validates the interface name before invoking ip(8).
// Returns false and logs on any failure.
bool configure_can_port(const std::string& port_id, uint32_t bitrate);

// Bring `port_id` administratively down (for a disabled assignment).
void disable_can_port(const std::string& port_id);
