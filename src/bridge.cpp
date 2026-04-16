#include "bridge.hpp"

#include "config.hpp"
#include "dhcp.hpp"
#include "proto_io.hpp"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

extern "C" {
#include "journeyos_device.pb-c.h"
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int      HEARTBEAT_INTERVAL_S = 30;
static constexpr int      RECONNECT_DELAY_S    = 10;
static constexpr uint32_t PROTOCOL_VERSION     = 1;

// UDP datagram is exactly 20 ASCII chars:
//   CCCC = 4 hex chars (lower 16 bits of CAN ID)
//   AA..HH = 8 × 2 hex chars for data bytes
static constexpr size_t DGRAM_LEN = 20;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t uptime_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u
         + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

static std::string read_mac(const std::string& iface) {
    std::string path = "/sys/class/net/" + iface + "/address";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "00:00:00:00:00:00";
    char buf[32] = {};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return "00:00:00:00:00:00"; }
    fclose(f);
    // strip newline
    for (char& c : buf) if (c == '\n' || c == '\r') { c = '\0'; break; }
    return buf;
}

// Open a SocketCAN raw socket bound to `iface`.
static int open_can_socket(const std::string& iface) {
    int fd = socket(AF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        fprintf(stderr, "can: socket(): %s\n", strerror(errno));
        return -1;
    }
    struct ifreq ifr{};
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "can: ioctl SIOCGIFINDEX '%s': %s\n",
                iface.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_can addr{};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "can: bind '%s': %s\n", iface.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// Open a connected UDP socket aimed at `hu_ip`:`port`.
static int open_udp_socket(const std::string& hu_ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("udp: socket"); return -1; }

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, hu_ip.c_str(), &dst.sin_addr) != 1) {
        fprintf(stderr, "udp: inet_pton failed for '%s'\n", hu_ip.c_str());
        close(fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
        perror("udp: connect");
        close(fd);
        return -1;
    }
    return fd;
}

// Connect TCP socket to `ip`:`port`. Returns fd or -1.
static int tcp_connect(const std::string& ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("tcp: socket"); return -1; }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "tcp: inet_pton '%s': %s\n", ip.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "tcp: connect %s:%u: %s\n",
                ip.c_str(), port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// Protobuf helpers
// ---------------------------------------------------------------------------

static bool send_announce(int tcp_fd,
                          const AppConfig& cfg,
                          const std::string& device_id) {
    // Build the udp_ports array from the config
    std::vector<uint32_t> ports;
    ports.reserve(cfg.buses.size());
    for (auto& [name, bus] : cfg.buses)
        ports.push_back(bus.port);

    Journeyos__Device__DeviceAnnounce ann = JOURNEYOS__DEVICE__DEVICE_ANNOUNCE__INIT;
    ann.protocol_version = PROTOCOL_VERSION;
    ann.type             = JOURNEYOS__DEVICE__DEVICE_TYPE__DEVICE_CANBUS;
    ann.firmware_version = const_cast<char*>(FIRMWARE_VERSION);
    ann.device_id        = const_cast<char*>(device_id.c_str());
    ann.n_udp_ports      = ports.size();
    ann.udp_ports        = ports.data();
    ann.config_hash      = const_cast<char*>(cfg.sha256_hex.c_str());

    Journeyos__Device__Envelope env = JOURNEYOS__DEVICE__ENVELOPE__INIT;
    env.payload_case = JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_ANNOUNCE;
    env.announce     = &ann;

    return send_envelope(tcp_fd, env);
}

static bool send_heartbeat(int tcp_fd, const std::string& device_id) {
    Journeyos__Device__Heartbeat hb = JOURNEYOS__DEVICE__HEARTBEAT__INIT;
    hb.device_id = const_cast<char*>(device_id.c_str());
    hb.uptime_ms = uptime_ms();

    Journeyos__Device__Envelope env = JOURNEYOS__DEVICE__ENVELOPE__INIT;
    env.payload_case = JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_HEARTBEAT;
    env.heartbeat    = &hb;

    return send_envelope(tcp_fd, env);
}

// ---------------------------------------------------------------------------
// Registration outcome
// ---------------------------------------------------------------------------

enum class RegResult { OK, UPDATE_REQUIRED, REJECTED, ERROR };

struct RegOutcome {
    RegResult result;
    uint32_t  assigned_id = 0;
};

// Performs the DeviceAnnounce → DeviceAck exchange.
// On UPDATE_REQUIRED, also reads the ConfigPush and writes the new config.
static RegOutcome do_register(int tcp_fd,
                              const AppConfig& cfg,
                              const std::string& device_id) {
    if (!send_announce(tcp_fd, cfg, device_id))
        return {RegResult::ERROR};

    EnvelopePtr env = recv_envelope(tcp_fd);
    if (!env) return {RegResult::ERROR};

    if (env->payload_case != JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_ACK) {
        fprintf(stderr, "reg: expected DeviceAck, got payload_case=%d\n",
                env->payload_case);
        return {RegResult::ERROR};
    }

    auto* ack = env->ack;
    switch (ack->status) {
    case JOURNEYOS__DEVICE__DEVICE_ACK__STATUS__OK:
        return {RegResult::OK, ack->assigned_id};

    case JOURNEYOS__DEVICE__DEVICE_ACK__STATUS__REJECTED:
        fprintf(stderr, "reg: REJECTED by HU\n");
        return {RegResult::REJECTED};

    case JOURNEYOS__DEVICE__DEVICE_ACK__STATUS__UPDATE_REQUIRED: {
        fprintf(stderr, "reg: UPDATE_REQUIRED — waiting for ConfigPush\n");
        EnvelopePtr cfg_env = recv_envelope(tcp_fd);
        if (!cfg_env ||
            cfg_env->payload_case != JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_CONFIG_PUSH) {
            fprintf(stderr, "reg: expected ConfigPush after UPDATE_REQUIRED\n");
            return {RegResult::ERROR};
        }
        auto* push = cfg_env->config_push;
        if (push->config_data.len == 0) {
            // download_url-only push: not implemented
            fprintf(stderr,
                    "reg: ConfigPush has no inline data (download_url='%s') — "
                    "fetch not implemented\n",
                    push->download_url ? push->download_url : "");
            return {RegResult::ERROR};
        }
        if (!write_config(CONFIG_PATH,
                          push->config_data.data,
                          push->config_data.len)) {
            fprintf(stderr, "reg: failed to write new config\n");
            return {RegResult::ERROR};
        }
        fprintf(stderr, "reg: new config written (hash: %s)\n",
                push->config_hash ? push->config_hash : "");
        return {RegResult::UPDATE_REQUIRED};
    }

    default:
        fprintf(stderr, "reg: unknown ack status %d\n", ack->status);
        return {RegResult::ERROR};
    }
}

// ---------------------------------------------------------------------------
// CAN → UDP forwarding helpers
// ---------------------------------------------------------------------------

struct BusSocket {
    std::string name;
    int         can_fd;
    int         udp_fd;
};

static std::vector<BusSocket> open_bus_sockets(const AppConfig& cfg,
                                                const std::string& hu_ip) {
    std::vector<BusSocket> sockets;
    sockets.reserve(cfg.buses.size());

    for (auto& [name, bus] : cfg.buses) {
        int can_fd = open_can_socket(bus.iface);
        if (can_fd < 0) {
            fprintf(stderr, "bus: skipping '%s' (can socket failed)\n", name.c_str());
            continue;
        }
        int udp_fd = open_udp_socket(hu_ip, bus.port);
        if (udp_fd < 0) {
            close(can_fd);
            fprintf(stderr, "bus: skipping '%s' (udp socket failed)\n", name.c_str());
            continue;
        }
        sockets.push_back({name, can_fd, udp_fd});
        fprintf(stderr, "bus: '%s' → %s → %s:%u\n",
                name.c_str(), bus.iface.c_str(), hu_ip.c_str(), bus.port);
    }
    return sockets;
}

static void close_bus_sockets(std::vector<BusSocket>& sockets) {
    for (auto& s : sockets) {
        close(s.can_fd);
        close(s.udp_fd);
    }
    sockets.clear();
}

// Read one CAN frame and send it as a 20-byte ASCII UDP datagram.
// Format: CCCCAABBCCDDEEFFGGHH (CAN ID lower 16 bits + 8 data bytes, uppercase hex)
static void forward_can_frame(const BusSocket& bus) {
    struct can_frame frame{};
    ssize_t n = read(bus.can_fd, &frame, sizeof(frame));
    if (n < static_cast<ssize_t>(sizeof(struct can_frame))) return;

    // Pad DLC < 8 with zeros (format is always 8 bytes)
    char dgram[DGRAM_LEN + 1];
    snprintf(dgram, sizeof(dgram),
             "%04X%02X%02X%02X%02X%02X%02X%02X%02X",
             static_cast<unsigned>(frame.can_id & 0xFFFFu),
             frame.data[0], frame.data[1], frame.data[2], frame.data[3],
             frame.data[4], frame.data[5], frame.data[6], frame.data[7]);

    send(bus.udp_fd, dgram, DGRAM_LEN, 0);
}

// ---------------------------------------------------------------------------
// Handle an incoming ConfigPush on the live TCP connection
// ---------------------------------------------------------------------------

// Returns true if config was updated (trigger reconnect)
static bool handle_config_push(Journeyos__Device__ConfigPush* push) {
    if (!push || push->config_data.len == 0) return false;
    if (!write_config(CONFIG_PATH,
                      push->config_data.data,
                      push->config_data.len)) {
        fprintf(stderr, "bridge: failed to write hot-pushed config\n");
        return false;
    }
    fprintf(stderr, "bridge: hot config push applied (hash: %s)\n",
            push->config_hash ? push->config_hash : "");
    return true;
}

// ---------------------------------------------------------------------------
// Main poll() loop
// ---------------------------------------------------------------------------

// Returns false if the TCP connection dropped (reconnect needed),
//         true  if the HU pushed a new config (full restart needed).
static bool run_poll_loop(int tcp_fd,
                          const std::string& device_id,
                          std::vector<BusSocket>& buses) {
    // fds layout: [0]=TCP, [1..n]=CAN
    const size_t n_fds = 1 + buses.size();
    std::vector<struct pollfd> fds(n_fds);

    fds[0].fd     = tcp_fd;
    fds[0].events = POLLIN;
    for (size_t i = 0; i < buses.size(); ++i) {
        fds[i + 1].fd     = buses[i].can_fd;
        fds[i + 1].events = POLLIN;
    }

    auto last_hb = std::chrono::steady_clock::now();
    const auto hb_interval = std::chrono::seconds(HEARTBEAT_INTERVAL_S);

    while (true) {
        using namespace std::chrono;
        auto now      = steady_clock::now();
        auto next_hb  = last_hb + hb_interval;
        int  timeout  = static_cast<int>(
            duration_cast<milliseconds>(next_hb - now).count());
        if (timeout < 0) timeout = 0;

        int ret = poll(fds.data(), static_cast<nfds_t>(n_fds), timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return false;
        }

        // Heartbeat due?
        now = steady_clock::now();
        if (now >= next_hb) {
            if (!send_heartbeat(tcp_fd, device_id)) return false;
            last_hb = now;
        }

        if (ret == 0) continue;

        // TCP events
        if (fds[0].revents & (POLLHUP | POLLERR | POLLRDHUP)) {
            fprintf(stderr, "bridge: TCP connection lost\n");
            return false;
        }
        if (fds[0].revents & POLLIN) {
            EnvelopePtr env = recv_envelope(tcp_fd);
            if (!env) return false;
            if (env->payload_case ==
                JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_CONFIG_PUSH) {
                if (handle_config_push(env->config_push)) return true; // restart
            }
        }

        // CAN events
        for (size_t i = 0; i < buses.size(); ++i) {
            if (fds[i + 1].revents & POLLIN)
                forward_can_frame(buses[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

int run_bridge(const std::string& primary_iface,
               const std::map<std::string, std::string>& iface_map) {
    while (true) {
        // --- 1. Load config ---
        auto cfg_opt = load_config(iface_map);
        if (!cfg_opt) {
            fprintf(stderr, "bridge: config load failed, retrying in %ds\n",
                    RECONNECT_DELAY_S);
            sleep(RECONNECT_DELAY_S);
            continue;
        }
        AppConfig cfg = std::move(*cfg_opt);

        // --- 2. Discover HU ---
        auto hu_opt = discover_hu(primary_iface);
        if (!hu_opt) {
            fprintf(stderr, "bridge: HU discovery failed, retrying in %ds\n",
                    RECONNECT_DELAY_S);
            sleep(RECONNECT_DELAY_S);
            continue;
        }
        HUInfo hu = *hu_opt;
        fprintf(stderr, "bridge: HU at %s port %u\n",
                hu.ip.c_str(), hu.mgmt_port);

        // --- 3. Get device_id (MAC) ---
        const std::string device_id = read_mac(primary_iface);

        // --- 4. Connect TCP ---
        int tcp_fd = tcp_connect(hu.ip, hu.mgmt_port);
        if (tcp_fd < 0) {
            fprintf(stderr, "bridge: TCP connect failed, retrying in %ds\n",
                    RECONNECT_DELAY_S);
            sleep(RECONNECT_DELAY_S);
            continue;
        }

        // --- 5 & 6. Registration ---
        auto outcome = do_register(tcp_fd, cfg, device_id);
        if (outcome.result == RegResult::REJECTED) {
            fprintf(stderr, "bridge: permanently rejected by HU — exiting\n");
            close(tcp_fd);
            return 1;
        }
        if (outcome.result == RegResult::UPDATE_REQUIRED ||
            outcome.result == RegResult::ERROR) {
            close(tcp_fd);
            // UPDATE_REQUIRED: config was updated — restart from step 1.
            // ERROR: transient — retry after delay.
            if (outcome.result == RegResult::ERROR)
                sleep(RECONNECT_DELAY_S);
            continue;
        }

        fprintf(stderr, "bridge: registered (assigned_id=%u)\n",
                outcome.assigned_id);

        // --- 7. Open CAN/UDP sockets ---
        std::vector<BusSocket> buses = open_bus_sockets(cfg, hu.ip);
        if (buses.empty()) {
            fprintf(stderr, "bridge: no CAN sockets opened\n");
            close(tcp_fd);
            sleep(RECONNECT_DELAY_S);
            continue;
        }

        // --- 8. Poll loop ---
        bool config_updated = run_poll_loop(tcp_fd, device_id, buses);

        close_bus_sockets(buses);
        close(tcp_fd);

        if (!config_updated) {
            fprintf(stderr, "bridge: TCP dropped — reconnecting in %ds\n",
                    RECONNECT_DELAY_S);
            sleep(RECONNECT_DELAY_S);
        }
        // Either way, loop back to re-register
    }
}