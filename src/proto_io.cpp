#include "proto_io.hpp"

#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

static constexpr uint32_t MAX_PROTO_PAYLOAD = 1u << 20; // 1 MiB guard

bool read_exact(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t rem = n;
    while (rem > 0) {
        ssize_t r = read(fd, p, rem);
        if (r <= 0) {
            if (r == 0) fprintf(stderr, "proto_io: connection closed\n");
            else        fprintf(stderr, "proto_io: read: %s\n", strerror(errno));
            return false;
        }
        p   += r;
        rem -= static_cast<size_t>(r);
    }
    return true;
}

bool write_exact(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t rem = n;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w <= 0) {
            fprintf(stderr, "proto_io: write: %s\n", strerror(errno));
            return false;
        }
        p   += w;
        rem -= static_cast<size_t>(w);
    }
    return true;
}

bool send_envelope(int fd, const Journeyos__Device__Envelope& env) {
    size_t len = journeyos__device__envelope__get_packed_size(&env);
    std::vector<uint8_t> buf(len);
    journeyos__device__envelope__pack(&env, buf.data());

    uint32_t net_len = htonl(static_cast<uint32_t>(len));
    return write_exact(fd, &net_len, 4) && write_exact(fd, buf.data(), len);
}

EnvelopePtr recv_envelope(int fd) {
    uint32_t net_len = 0;
    if (!read_exact(fd, &net_len, 4)) return nullptr;

    uint32_t len = ntohl(net_len);
    if (len == 0 || len > MAX_PROTO_PAYLOAD) {
        fprintf(stderr, "proto_io: bad payload length %u\n", len);
        return nullptr;
    }

    std::vector<uint8_t> buf(len);
    if (!read_exact(fd, buf.data(), len)) return nullptr;

    Journeyos__Device__Envelope* env =
        journeyos__device__envelope__unpack(nullptr, len, buf.data());
    if (!env) {
        fprintf(stderr, "proto_io: unpack failed\n");
        return nullptr;
    }
    return EnvelopePtr(env);
}