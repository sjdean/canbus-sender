#include "can_probe.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

// ARPHRD_CAN = 280 (from linux/if_arp.h; safe to hard-code)
static constexpr int ARPHRD_CAN = 280;

// CAN_CTRLMODE_FD = bit 5 = 0x20 (from linux/can/netlink.h)
static constexpr unsigned int CTRLMODE_FD_BIT = 0x20u;

// ---------------------------------------------------------------------------
// sysfs helpers
// ---------------------------------------------------------------------------

static int sysfs_read_int(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return -1;
    int v = -1;
    fscanf(f, "%d", &v);
    fclose(f);
    return v;
}

static uint32_t sysfs_read_uint(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    uint32_t v = 0;
    fscanf(f, "%u", &v);
    fclose(f);
    return v;
}

static std::string sysfs_read_str(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[64] = {};
    fscanf(f, "%63s", buf);
    fclose(f);
    return buf;
}

static bool path_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// ---------------------------------------------------------------------------
// Per-interface capability detection
// ---------------------------------------------------------------------------

static bool detect_fd_capable(const std::string& iface) {
    // Primary: ctrlmode_supported carries the CTRLMODE_FD bit when driver supports FD.
    // Not present on older kernels — fall back to checking for data_bitrate attribute.
    std::string sup = "/sys/class/net/" + iface + "/can/ctrlmode_supported";
    FILE* f = fopen(sup.c_str(), "r");
    if (f) {
        unsigned int v = 0;
        fscanf(f, "%x", &v);
        fclose(f);
        return (v & CTRLMODE_FD_BIT) != 0;
    }
    // Fallback: data_bitrate sysfs entry is only created for FD-capable ports
    return path_exists("/sys/class/net/" + iface + "/can/data_bitrate");
}

static bool detect_link_active(const std::string& iface) {
    // CAN interfaces report "up" or "unknown" when administratively up.
    // "down" means the interface has never been brought up or was explicitly downed.
    std::string state = sysfs_read_str("/sys/class/net/" + iface + "/operstate");
    return (state == "up" || state == "unknown");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<CanPortProbe> enumerate_can_ports() {
    std::vector<CanPortProbe> result;

    DIR* dir = opendir("/sys/class/net");
    if (!dir) {
        perror("can_probe: opendir /sys/class/net");
        return result;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        const std::string iface = ent->d_name;

        if (sysfs_read_int("/sys/class/net/" + iface + "/type") != ARPHRD_CAN)
            continue;

        CanPortProbe p;
        p.port_id    = iface;
        p.bitrate    = sysfs_read_uint("/sys/class/net/" + iface + "/can/bitrate");
        p.fd_capable = detect_fd_capable(iface);
        p.link_active = detect_link_active(iface);
        result.push_back(std::move(p));
    }
    closedir(dir);

    std::sort(result.begin(), result.end(),
        [](const CanPortProbe& a, const CanPortProbe& b) {
            return a.port_id < b.port_id;
        });
    return result;
}

// ---------------------------------------------------------------------------
// Validate that a name is safe to pass to ip(8): alphanumeric/hyphen/underscore,
// 1-15 chars. CAN interface names are always in this set.
// ---------------------------------------------------------------------------
static bool valid_iface_name(const std::string& name) {
    if (name.empty() || name.size() > 15) return false;
    for (unsigned char c : name)
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    return true;
}

bool configure_can_port(const std::string& port_id, uint32_t bitrate) {
    if (!valid_iface_name(port_id)) {
        fprintf(stderr, "can_probe: invalid interface name '%s'\n", port_id.c_str());
        return false;
    }
    if (bitrate == 0) {
        fprintf(stderr, "can_probe: bitrate=0 for '%s', skipping\n", port_id.c_str());
        return false;
    }

    char cmd[256];

    // Must be down before reconfiguring
    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", port_id.c_str());
    system(cmd);

    snprintf(cmd, sizeof(cmd),
             "ip link set %s type can bitrate %u", port_id.c_str(), bitrate);
    if (system(cmd) != 0) {
        fprintf(stderr, "can_probe: failed to set bitrate on '%s'\n", port_id.c_str());
        return false;
    }

    snprintf(cmd, sizeof(cmd), "ip link set %s up", port_id.c_str());
    if (system(cmd) != 0) {
        fprintf(stderr, "can_probe: failed to bring up '%s'\n", port_id.c_str());
        return false;
    }

    fprintf(stderr, "can_probe: %s → %u bps\n", port_id.c_str(), bitrate);
    return true;
}

void disable_can_port(const std::string& port_id) {
    if (!valid_iface_name(port_id)) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", port_id.c_str());
    system(cmd);
    fprintf(stderr, "can_probe: %s disabled\n", port_id.c_str());
}
