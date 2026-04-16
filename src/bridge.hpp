#pragma once

#include "config.hpp"
#include "dhcp.hpp"

#include <map>
#include <string>

// Run the full CAN-bridge lifecycle:
//  1. Registration loop (reconnects after failure).
//  2. poll() loop forwarding CAN frames as UDP datagrams and sending TCP heartbeats.
//
// `primary_iface`  – network interface that holds the DHCP lease (e.g. "eth0").
// `iface_map`      – bus-name → CAN interface mapping from the command line.
//
// This function does not return under normal operation.
// Returns non-zero on fatal error (e.g. REJECTED by HU).
int run_bridge(const std::string& primary_iface,
               const std::map<std::string, std::string>& iface_map);