#pragma once

#include <string>

// Run the full CAN-bridge lifecycle.
//
// `primary_iface` — network interface that holds the DHCP lease (e.g. "eth0").
//                   Also used to read the device MAC address.
//
// Behaviour:
//   - Enumerates physical CAN ports and reports them in DeviceAnnounce.
//   - Loads any stored port assignments from disk.
//   - If the HU is reachable: performs full registration, stores the received
//     CanPortAssignment config, applies bitrates, and starts forwarding.
//   - If the HU is unreachable BUT a stored config exists: starts forwarding
//     with the last-known config (degraded mode) and retries registration in
//     the background every 60 seconds.
//   - If the HU is unreachable AND no stored config exists: waits and retries
//     indefinitely until the HU responds (virgin device cannot operate alone).
//
// Does not return under normal operation.
// Returns non-zero on a fatal, unrecoverable error (e.g. permanently REJECTED).
int run_bridge(const std::string& primary_iface);
