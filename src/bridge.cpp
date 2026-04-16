#include "bridge.hpp"

#include "can_probe.hpp"
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
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "journeyos_device.pb-c.h"
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int    HEARTBEAT_INTERVAL_S  = 30;
static constexpr int    RECONNECT_INTERVAL_S  = 60;  // degraded-mode retry
static constexpr int    RECONNECT_DELAY_S     = 10;  // delay after transient failure
static constexpr int    CONNECT_TIMEOUT_MS    = 5000; // non-blocking TCP connect timeout
static constexpr uint32_t PROTOCOL_VERSION    = 1;
static constexpr size_t DGRAM_LEN             = 20;   // ASCII UDP datagram length

// ---------------------------------------------------------------------------
// Uptime / MAC helpers
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
    fgets(buf, sizeof(buf), f);
    fclose(f);
    for (char& c : buf) if (c == '\n' || c == '\r') { c = '\0'; break; }
    return buf;
}

// ---------------------------------------------------------------------------
// Non-blocking TCP connect with timeout
// ---------------------------------------------------------------------------

static int tcp_connect_nb(const std::string& ip, uint16_t port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        close(fd); return -1;
    }

    int r = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == 0) {
        // Immediate success (loopback / already connected)
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        return fd;
    }
    if (errno != EINPROGRESS) { close(fd); return -1; }

    struct pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, timeout_ms) <= 0) { close(fd); return -1; }

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        close(fd); return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

// ---------------------------------------------------------------------------
// CAN + UDP socket management
// ---------------------------------------------------------------------------

static int open_can_socket(const std::string& iface) {
    int fd = socket(AF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) { perror("can: socket"); return -1; }

    struct ifreq ifr{};
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "can: SIOCGIFINDEX '%s': %s\n", iface.c_str(), strerror(errno));
        close(fd); return -1;
    }

    struct sockaddr_can addr{};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "can: bind '%s': %s\n", iface.c_str(), strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

static int open_udp_socket(const std::string& hu_ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("udp: socket"); return -1; }

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, hu_ip.c_str(), &dst.sin_addr) != 1) {
        close(fd); return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
        perror("udp: connect"); close(fd); return -1;
    }
    return fd;
}

struct BusSocket {
    std::string bus_name;
    std::string port_id;
    int         can_fd = -1;
    int         udp_fd = -1;
};

static std::vector<BusSocket> open_bus_sockets(const StoredConfig& cfg,
                                                const std::string& hu_ip) {
    std::vector<BusSocket> result;
    result.reserve(cfg.ports.size());
    for (const auto& p : cfg.ports) {
        if (!p.enabled) continue;
        int can_fd = open_can_socket(p.port_id);
        if (can_fd < 0) {
            fprintf(stderr, "bridge: skipping '%s' (CAN socket failed)\n", p.port_id.c_str());
            continue;
        }
        int udp_fd = open_udp_socket(hu_ip, p.udp_port);
        if (udp_fd < 0) {
            close(can_fd);
            fprintf(stderr, "bridge: skipping '%s' (UDP socket failed)\n", p.port_id.c_str());
            continue;
        }
        fprintf(stderr, "bridge: %s (%s) → UDP %s:%u\n",
                p.port_id.c_str(), p.bus_name.c_str(), hu_ip.c_str(), p.udp_port);
        result.push_back({p.bus_name, p.port_id, can_fd, udp_fd});
    }
    return result;
}

static void close_bus_sockets(std::vector<BusSocket>& sockets) {
    for (auto& s : sockets) {
        if (s.can_fd >= 0) close(s.can_fd);
        if (s.udp_fd >= 0) close(s.udp_fd);
    }
    sockets.clear();
}

// Read one CAN frame, format as 20-byte ASCII, send UDP.
static void forward_can_frame(const BusSocket& bus) {
    struct can_frame frame{};
    if (read(bus.can_fd, &frame, sizeof(frame)) <
        static_cast<ssize_t>(sizeof(struct can_frame))) return;

    char dgram[DGRAM_LEN + 1];
    snprintf(dgram, sizeof(dgram),
             "%04X%02X%02X%02X%02X%02X%02X%02X%02X",
             static_cast<unsigned>(frame.can_id & 0xFFFFu),
             frame.data[0], frame.data[1], frame.data[2], frame.data[3],
             frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
    send(bus.udp_fd, dgram, DGRAM_LEN, 0);
}

// ---------------------------------------------------------------------------
// Apply stored config: configure bitrates, bring down disabled ports
// ---------------------------------------------------------------------------

static void apply_port_config(const StoredConfig& cfg) {
    for (const auto& p : cfg.ports) {
        if (!p.enabled) {
            disable_can_port(p.port_id);
        } else if (p.bitrate > 0) {
            configure_can_port(p.port_id, p.bitrate);
        }
    }
}

// ---------------------------------------------------------------------------
// Protobuf helpers
// ---------------------------------------------------------------------------

static bool send_announce(int tcp_fd,
                           const std::optional<StoredConfig>& stored,
                           const std::vector<CanPortProbe>& probed,
                           const std::string& device_id) {
    // UDP ports currently in use (from stored config — for HU reference)
    std::vector<uint32_t> udp_ports;
    if (stored) {
        for (const auto& p : stored->ports)
            if (p.enabled) udp_ports.push_back(p.udp_port);
    }

    // Build CanPortInfo array from live kernel probe
    std::vector<Journeyos__Device__CanPortInfo>  info_storage(probed.size());
    std::vector<Journeyos__Device__CanPortInfo*> info_ptrs(probed.size());
    for (size_t i = 0; i < probed.size(); ++i) {
        info_storage[i]               = JOURNEYOS__DEVICE__CAN_PORT_INFO__INIT;
        info_storage[i].port_id       = const_cast<char*>(probed[i].port_id.c_str());
        info_storage[i].bitrate       = probed[i].bitrate;
        info_storage[i].fd_capable    = probed[i].fd_capable;
        info_storage[i].link_active   = probed[i].link_active;
        info_ptrs[i]                  = &info_storage[i];
    }

    const std::string& hash = stored ? stored->sha256_hex : std::string{};

    Journeyos__Device__DeviceAnnounce ann = JOURNEYOS__DEVICE__DEVICE_ANNOUNCE__INIT;
    ann.protocol_version = PROTOCOL_VERSION;
    ann.type             = JOURNEYOS__DEVICE__DEVICE_TYPE__DEVICE_CANBUS;
    ann.firmware_version = const_cast<char*>(FIRMWARE_VERSION);
    ann.device_id        = const_cast<char*>(device_id.c_str());
    ann.n_udp_ports      = udp_ports.size();
    ann.udp_ports        = udp_ports.data();
    ann.config_hash      = const_cast<char*>(hash.c_str());
    ann.n_can_ports      = info_ptrs.size();
    ann.can_ports        = info_ptrs.data();

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

// Build a StoredConfig from the can_ports in a DeviceAck.
static StoredConfig config_from_ack(uint32_t assigned_id,
                                     const Journeyos__Device__DeviceAck* ack) {
    StoredConfig cfg;
    cfg.assigned_id = assigned_id;
    for (size_t i = 0; i < ack->n_can_ports; ++i) {
        const auto* a = ack->can_ports[i];
        PortAssignment pa;
        pa.port_id     = a->port_id      ? a->port_id      : "";
        pa.bus_name    = a->bus_name     ? a->bus_name     : "";
        pa.description = a->description  ? a->description  : "";
        pa.bitrate     = a->bitrate;
        pa.udp_port    = static_cast<uint16_t>(a->udp_port);
        pa.enabled     = a->enabled;
        cfg.ports.push_back(std::move(pa));
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

enum class RegResult { OK, UPDATE_REQUIRED, REJECTED, ERROR };

struct RegOutcome {
    RegResult   result;
    StoredConfig new_config;   // populated on OK
    bool         config_changed = false;
};

static RegOutcome do_register(int tcp_fd,
                               const std::optional<StoredConfig>& stored,
                               const std::vector<CanPortProbe>& can_ports,
                               const std::string& device_id) {
    if (!send_announce(tcp_fd, stored, can_ports, device_id))
        return {RegResult::ERROR};

    EnvelopePtr env = recv_envelope(tcp_fd);
    if (!env) return {RegResult::ERROR};

    if (env->payload_case != JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_ACK) {
        fprintf(stderr, "reg: expected DeviceAck, got payload_case=%d\n",
                env->payload_case);
        return {RegResult::ERROR};
    }

    const auto* ack = env->ack;

    switch (ack->status) {
    case JOURNEYOS__DEVICE__DEVICE_ACK__STATUS__OK: {
        StoredConfig new_cfg = config_from_ack(ack->assigned_id, ack);
        // Determine if config changed by comparing serialised hashes
        bool changed = !stored || (stored->sha256_hex != new_cfg.sha256_hex);
        // sha256_hex is not yet computed (save_stored_config does that);
        // compare on assigned_id + port count + first port as quick check
        changed = !stored
                  || stored->assigned_id   != new_cfg.assigned_id
                  || stored->ports.size()  != new_cfg.ports.size();
        if (!changed && stored) {
            for (size_t i = 0; i < stored->ports.size(); ++i) {
                const auto& a = stored->ports[i];
                const auto& b = new_cfg.ports[i];
                if (a.port_id != b.port_id || a.bitrate != b.bitrate
                    || a.udp_port != b.udp_port || a.enabled != b.enabled) {
                    changed = true; break;
                }
            }
        }
        return {RegResult::OK, std::move(new_cfg), changed};
    }

    case JOURNEYOS__DEVICE__DEVICE_ACK__STATUS__REJECTED:
        fprintf(stderr, "reg: REJECTED by HU\n");
        return {RegResult::REJECTED};

    case JOURNEYOS__DEVICE__DEVICE_ACK__STATUS__UPDATE_REQUIRED: {
        fprintf(stderr, "reg: UPDATE_REQUIRED — waiting for ConfigPush\n");
        EnvelopePtr ce = recv_envelope(tcp_fd);
        if (!ce || ce->payload_case != JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_CONFIG_PUSH) {
            fprintf(stderr, "reg: expected ConfigPush after UPDATE_REQUIRED\n");
            return {RegResult::ERROR};
        }
        const auto* push = ce->config_push;
        if (push->config_data.len == 0) {
            fprintf(stderr, "reg: ConfigPush has no inline data (download not implemented)\n");
            return {RegResult::ERROR};
        }
        // Write raw bytes — assumed to be the new port_assignments JSON
        FILE* f = fopen(PORT_ASSIGNMENTS_PATH, "wb");
        if (!f || fwrite(push->config_data.data, 1, push->config_data.len, f) !=
                  push->config_data.len) {
            if (f) fclose(f);
            fprintf(stderr, "reg: failed to write pushed config\n");
            return {RegResult::ERROR};
        }
        fclose(f);
        fprintf(stderr, "reg: config updated from ConfigPush (hash: %s)\n",
                push->config_hash ? push->config_hash : "");
        return {RegResult::UPDATE_REQUIRED};
    }

    default:
        fprintf(stderr, "reg: unknown ack status %d\n", ack->status);
        return {RegResult::ERROR};
    }
}

// ---------------------------------------------------------------------------
// Handle a live ConfigPush arriving during normal operation
// ---------------------------------------------------------------------------

static bool handle_config_push(Journeyos__Device__ConfigPush* push) {
    if (!push || push->config_data.len == 0) return false;
    FILE* f = fopen(PORT_ASSIGNMENTS_PATH, "wb");
    if (!f || fwrite(push->config_data.data, 1, push->config_data.len, f) !=
              push->config_data.len) {
        if (f) fclose(f);
        fprintf(stderr, "bridge: failed to write hot config push\n");
        return false;
    }
    fclose(f);
    fprintf(stderr, "bridge: hot config push applied (hash: %s)\n",
            push->config_hash ? push->config_hash : "");
    return true;
}

// ---------------------------------------------------------------------------
// Unified poll() loop
//
// Handles two modes distinguished by tcp_fd:
//   tcp_fd >= 0  → REGISTERED: CAN forwarding + 30 s TCP heartbeat
//   tcp_fd == -1 → DEGRADED:   CAN forwarding + 60 s reconnect attempt
//
// Return values:
//   true  → caller should restart the outer registration loop immediately
//   false → fatal / unrecoverable; caller should retry after a delay
// ---------------------------------------------------------------------------

static bool run_poll_loop(int              tcp_fd,
                          const std::string& device_id,
                          const HUInfo&    hu,
                          std::vector<BusSocket>& buses) {
    const bool registered = (tcp_fd >= 0);
    const size_t n_can    = buses.size();
    const size_t tcp_slot = 0;
    const size_t can_off  = registered ? 1u : 0u;
    const size_t n_fds    = can_off + n_can;

    std::vector<struct pollfd> fds(n_fds);
    if (registered) {
        fds[tcp_slot].fd     = tcp_fd;
        fds[tcp_slot].events = POLLIN;
    }
    for (size_t i = 0; i < n_can; ++i) {
        fds[can_off + i].fd     = buses[i].can_fd;
        fds[can_off + i].events = POLLIN;
    }

    using namespace std::chrono;
    const int action_interval_s = registered ? HEARTBEAT_INTERVAL_S
                                             : RECONNECT_INTERVAL_S;
    auto last_action = steady_clock::now();
    const auto action_interval = seconds(action_interval_s);

    while (true) {
        auto now       = steady_clock::now();
        auto next_act  = last_action + action_interval;
        int  timeout   = static_cast<int>(
            duration_cast<milliseconds>(next_act - now).count());
        if (timeout < 0) timeout = 0;

        int ret = poll(fds.data(), static_cast<nfds_t>(n_fds), timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return false;
        }

        // Timer fired?
        now = steady_clock::now();
        if (now >= next_act) {
            if (registered) {
                if (!send_heartbeat(tcp_fd, device_id)) {
                    fprintf(stderr, "bridge: heartbeat failed — reconnecting\n");
                    return true; // restart registration
                }
            } else {
                // Degraded: probe whether HU is back
                int probe = tcp_connect_nb(hu.ip, hu.mgmt_port, CONNECT_TIMEOUT_MS);
                if (probe >= 0) {
                    close(probe);
                    fprintf(stderr, "bridge: HU is back — re-registering\n");
                    return true;
                }
                fprintf(stderr, "bridge: HU still unreachable, retrying in %ds\n",
                        RECONNECT_INTERVAL_S);
            }
            last_action = now;
        }

        if (ret == 0) continue;

        // TCP events (registered mode only)
        if (registered) {
            if (fds[tcp_slot].revents & (POLLHUP | POLLERR | POLLRDHUP)) {
                fprintf(stderr, "bridge: TCP connection lost\n");
                return true;
            }
            if (fds[tcp_slot].revents & POLLIN) {
                EnvelopePtr env = recv_envelope(tcp_fd);
                if (!env) return true;
                if (env->payload_case ==
                    JOURNEYOS__DEVICE__ENVELOPE__PAYLOAD_CONFIG_PUSH) {
                    if (handle_config_push(env->config_push)) return true;
                }
            }
        }

        // CAN events — hot path, no heap allocation
        for (size_t i = 0; i < n_can; ++i) {
            if (fds[can_off + i].revents & POLLIN)
                forward_can_frame(buses[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

int run_bridge(const std::string& primary_iface) {
    const std::string device_id = read_mac(primary_iface);

    while (true) {
        // ----------------------------------------------------------------
        // 1. Load stored config (may be absent on a virgin device)
        // ----------------------------------------------------------------
        auto stored = load_stored_config();

        // ----------------------------------------------------------------
        // 2. Enumerate physical CAN ports (always, even in degraded mode)
        // ----------------------------------------------------------------
        std::vector<CanPortProbe> can_probes = enumerate_can_ports();
        fprintf(stderr, "bridge: found %zu CAN port(s)\n", can_probes.size());
        for (const auto& p : can_probes)
            fprintf(stderr, "  %s  %u bps  FD=%s  active=%s\n",
                    p.port_id.c_str(), p.bitrate,
                    p.fd_capable  ? "yes" : "no",
                    p.link_active ? "yes" : "no");

        // ----------------------------------------------------------------
        // 3. Discover HU via DHCP lease
        // ----------------------------------------------------------------
        auto hu_opt = discover_hu(primary_iface);
        if (!hu_opt) {
            if (stored) {
                fprintf(stderr,
                        "bridge: HU discovery failed — running degraded on stored config\n");
                apply_port_config(*stored);
                std::vector<BusSocket> buses = open_bus_sockets(*stored, "0.0.0.0");
                // In degraded-no-HU mode we can't meaningfully retry (no IP).
                // Forward until something changes, then restart.
                if (!buses.empty()) {
                    HUInfo dummy{"0.0.0.0", 0};
                    run_poll_loop(-1, device_id, dummy, buses);
                    close_bus_sockets(buses);
                }
            } else {
                fprintf(stderr,
                        "bridge: HU discovery failed and no stored config — "
                        "retrying in %ds\n", RECONNECT_DELAY_S);
            }
            sleep(RECONNECT_DELAY_S);
            continue;
        }
        const HUInfo hu = *hu_opt;
        fprintf(stderr, "bridge: HU at %s port %u\n", hu.ip.c_str(), hu.mgmt_port);

        // ----------------------------------------------------------------
        // 4. Non-blocking TCP connect (CONNECT_TIMEOUT_MS)
        // ----------------------------------------------------------------
        int tcp_fd = tcp_connect_nb(hu.ip, hu.mgmt_port, CONNECT_TIMEOUT_MS);

        if (tcp_fd < 0) {
            if (stored) {
                fprintf(stderr,
                        "bridge: HU unreachable — forwarding with stored config "
                        "(will retry every %ds)\n", RECONNECT_INTERVAL_S);
                apply_port_config(*stored);
                std::vector<BusSocket> buses = open_bus_sockets(*stored, hu.ip);
                if (!buses.empty()) {
                    run_poll_loop(-1, device_id, hu, buses);
                    close_bus_sockets(buses);
                }
            } else {
                fprintf(stderr,
                        "bridge: HU unreachable and no stored config — "
                        "cannot operate; retrying in %ds\n", RECONNECT_DELAY_S);
                sleep(RECONNECT_DELAY_S);
            }
            continue;
        }

        // ----------------------------------------------------------------
        // 5 & 6. Registration handshake
        // ----------------------------------------------------------------
        RegOutcome outcome = do_register(tcp_fd, stored, can_probes, device_id);

        if (outcome.result == RegResult::REJECTED) {
            fprintf(stderr, "bridge: permanently REJECTED — exiting\n");
            close(tcp_fd);
            return 1;
        }
        if (outcome.result == RegResult::ERROR) {
            close(tcp_fd);
            sleep(RECONNECT_DELAY_S);
            continue;
        }
        if (outcome.result == RegResult::UPDATE_REQUIRED) {
            // config was written to disk by do_register; loop to reload
            close(tcp_fd);
            continue;
        }

        // outcome.result == OK
        fprintf(stderr, "bridge: registered (assigned_id=%u, %zu port(s) assigned)\n",
                outcome.new_config.assigned_id, outcome.new_config.ports.size());

        // ----------------------------------------------------------------
        // 7. Persist config if it changed
        // ----------------------------------------------------------------
        if (outcome.config_changed) {
            if (save_stored_config(outcome.new_config)) {
                fprintf(stderr, "bridge: stored config updated (hash: %s)\n",
                        outcome.new_config.sha256_hex.c_str());
            }
        }
        stored = std::move(outcome.new_config);

        // ----------------------------------------------------------------
        // 8. Apply bitrates / enable/disable ports
        // ----------------------------------------------------------------
        apply_port_config(*stored);

        // ----------------------------------------------------------------
        // 9. Open CAN + UDP sockets and enter poll loop
        // ----------------------------------------------------------------
        std::vector<BusSocket> buses = open_bus_sockets(*stored, hu.ip);
        if (buses.empty()) {
            fprintf(stderr, "bridge: no CAN sockets opened — reconnecting in %ds\n",
                    RECONNECT_DELAY_S);
            close(tcp_fd);
            sleep(RECONNECT_DELAY_S);
            continue;
        }

        run_poll_loop(tcp_fd, device_id, hu, buses);

        close_bus_sockets(buses);
        close(tcp_fd);
        sleep(RECONNECT_DELAY_S);
    }
}
