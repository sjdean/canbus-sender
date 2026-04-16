#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

extern "C" {
#include "journeyos_device.pb-c.h"
}

// Deleter for protobuf-c unpacked messages
struct ProtoFree {
    void operator()(Journeyos__Device__Envelope* p) const noexcept {
        if (p) journeyos__device__envelope__free_unpacked(p, nullptr);
    }
};
using EnvelopePtr = std::unique_ptr<Journeyos__Device__Envelope, ProtoFree>;

// Read exactly `n` bytes from fd. Returns false on EOF or error.
bool read_exact(int fd, void* buf, size_t n);

// Write exactly `n` bytes to fd. Returns false on error.
bool write_exact(int fd, const void* buf, size_t n);

// Send a length-prefixed (4-byte big-endian) serialised Envelope.
bool send_envelope(int fd, const Journeyos__Device__Envelope& env);

// Receive a length-prefixed Envelope. Returns nullptr on error/close.
// Max payload guarded to 1 MiB.
EnvelopePtr recv_envelope(int fd);