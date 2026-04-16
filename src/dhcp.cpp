#include "dhcp.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

// ---------------------------------------------------------------------------
// DHCP packet layout (RFC 2131)
//   0       : op
//   1       : htype
//   2       : hlen
//   3       : hops
//   4-7     : xid
//   8-9     : secs
//   10-11   : flags
//   12-15   : ciaddr
//   16-19   : yiaddr  (assigned IP – not the server)
//   20-23   : siaddr  (server IP – often 0; use option 54 instead)
//   24-27   : giaddr
//   28-43   : chaddr  (client HW addr)
//   44-107  : sname
//   108-235 : file
//   236-239 : magic cookie (0x63825363)
//   240+    : options (TLV; tag=0 pad, tag=255 end)
// ---------------------------------------------------------------------------

static constexpr size_t   BOOTP_SIZE          = 236;
static constexpr uint32_t DHCP_MAGIC_COOKIE   = 0x63825363u;
static constexpr uint8_t  OPT_PAD             = 0;
static constexpr uint8_t  OPT_END             = 255;
static constexpr uint8_t  OPT_SERVER_ID       = 54;
static constexpr uint8_t  OPT_VENDOR_SPECIFIC = 43;
static constexpr uint8_t  SIADDR_OFFSET       = 20;

static constexpr const char* LEASE_PATHS[] = {
    "/var/lib/dhcpcd/{}.lease",
    "/var/lib/dhcpcd5/{}.lease",
};

static std::string fmt_path(const char* tmpl, const std::string& iface) {
    std::string r = tmpl;
    auto pos = r.find("{}");
    if (pos != std::string::npos) r.replace(pos, 2, iface);
    return r;
}

static std::string ip4str(const uint8_t* p) {
    char buf[INET_ADDRSTRLEN];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return buf;
}

std::optional<HUInfo> discover_hu(const std::string& iface) {
    for (const char* tmpl : LEASE_PATHS) {
        std::string path = fmt_path(tmpl, iface);
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;

        std::vector<uint8_t> pkt(
            (std::istreambuf_iterator<char>(f)),
             std::istreambuf_iterator<char>());

        if (pkt.size() < BOOTP_SIZE + 4) {
            fprintf(stderr, "dhcp: lease file too short: %s\n", path.c_str());
            continue;
        }

        // Verify magic cookie
        uint32_t cookie;
        memcpy(&cookie, pkt.data() + BOOTP_SIZE, 4);
        if (ntohl(cookie) != DHCP_MAGIC_COOKIE) {
            fprintf(stderr, "dhcp: bad magic cookie in %s\n", path.c_str());
            continue;
        }

        std::string server_ip;
        std::string vendor_specific;

        size_t pos = BOOTP_SIZE + 4;
        while (pos < pkt.size()) {
            uint8_t tag = pkt[pos++];
            if (tag == OPT_PAD) continue;
            if (tag == OPT_END) break;
            if (pos >= pkt.size()) break;
            uint8_t len = pkt[pos++];
            if (pos + len > pkt.size()) break;

            if (tag == OPT_SERVER_ID && len == 4) {
                server_ip = ip4str(pkt.data() + pos);
            } else if (tag == OPT_VENDOR_SPECIFIC && len > 0) {
                vendor_specific.assign(
                    reinterpret_cast<const char*>(pkt.data() + pos), len);
            }
            pos += len;
        }

        // Fall back to siaddr if option 54 absent
        if (server_ip.empty())
            server_ip = ip4str(pkt.data() + SIADDR_OFFSET);

        if (server_ip.empty() || server_ip == "0.0.0.0") {
            fprintf(stderr, "dhcp: cannot determine server IP from %s\n", path.c_str());
            continue;
        }
        if (vendor_specific.empty()) {
            fprintf(stderr, "dhcp: option 43 absent in %s\n", path.c_str());
            continue;
        }

        // Parse "journeyos:<port>"
        static constexpr const char PREFIX[] = "journeyos:";
        if (vendor_specific.compare(0, sizeof(PREFIX) - 1, PREFIX) != 0) {
            fprintf(stderr, "dhcp: option 43 not journeyos format: '%s'\n",
                    vendor_specific.c_str());
            continue;
        }

        try {
            long port = std::stol(vendor_specific.substr(sizeof(PREFIX) - 1));
            if (port < 1 || port > 65535) throw std::out_of_range("port");
            return HUInfo{server_ip, static_cast<uint16_t>(port)};
        } catch (...) {
            fprintf(stderr, "dhcp: bad port in option 43: '%s'\n",
                    vendor_specific.c_str());
            continue;
        }
    }

    fprintf(stderr, "dhcp: no valid lease found for interface '%s'\n", iface.c_str());
    return std::nullopt;
}