#include "bridge.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --iface <primary_iface>\n"
        "\n"
        "  --iface  Network interface that holds the DHCP lease (e.g. eth0).\n"
        "           Also used to read the device MAC address.\n"
        "\n"
        "Port assignments (bus name, CAN interface, bitrate, UDP port) are\n"
        "received from the head unit on first boot and stored in:\n"
        "  /etc/journeyos/port_assignments.json\n"
        "\n"
        "The device will start forwarding from its stored config if the HU\n"
        "is unreachable at boot, and re-register as soon as the HU comes back.\n"
        "\n"
        "Example:\n"
        "  canbus-sender --iface eth0\n",
        prog);
}

int main(int argc, char** argv) {
    std::string primary_iface;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            primary_iface = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (primary_iface.empty()) {
        fprintf(stderr, "error: --iface is required\n");
        usage(argv[0]);
        return 1;
    }

    return run_bridge(primary_iface);
}
