#include "bridge.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --iface <primary_iface> --map <bus>=<can_iface> [--map ...]\n"
        "\n"
        "  --iface  Network interface that holds the DHCP lease (e.g. eth0)\n"
        "  --map    Map a config bus name to a Linux CAN interface\n"
        "           e.g. --map media=can0 --map diagnostic=can1\n"
        "\n"
        "Example:\n"
        "  canbus-sender --iface eth0 --map media=can0 --map diagnostic=can1\n",
        prog);
}

int main(int argc, char** argv) {
    std::string primary_iface;
    std::map<std::string, std::string> iface_map;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            primary_iface = argv[++i];
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            std::string arg = argv[++i];
            auto sep = arg.find('=');
            if (sep == std::string::npos || sep == 0 || sep + 1 == arg.size()) {
                fprintf(stderr, "bad --map value '%s' (expected bus=iface)\n",
                        arg.c_str());
                return 1;
            }
            iface_map[arg.substr(0, sep)] = arg.substr(sep + 1);
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
    if (iface_map.empty()) {
        fprintf(stderr, "error: at least one --map is required\n");
        usage(argv[0]);
        return 1;
    }

    return run_bridge(primary_iface, iface_map);
}