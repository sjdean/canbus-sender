#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct HUInfo {
    std::string ip;         // Head-unit IP address (the DHCP server)
    uint16_t    mgmt_port;  // Management TCP port from DHCP Option 43
};

// Parse the dhcpcd binary lease file for `iface` and extract the
// HU's IP (DHCP Server Identifier, option 54) and management port
// (Vendor-Specific, option 43: "journeyos:<port>").
//
// Tries paths in order:
//   /var/lib/dhcpcd/<iface>.lease
//   /var/lib/dhcpcd5/<iface>.lease
std::optional<HUInfo> discover_hu(const std::string& iface);