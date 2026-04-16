#include "config.hpp"

#include <openssl/evp.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Minimal JSON parser — handles exactly {"buses": {"name": port, ...}}
// ---------------------------------------------------------------------------

namespace {

void skip_ws(const std::string& s, size_t& p) {
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
}

std::string parse_str(const std::string& s, size_t& p) {
    if (p >= s.size() || s[p] != '"') throw std::runtime_error("expected '\"'");
    ++p;
    std::string r;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\') {
            ++p;
            if (p >= s.size()) throw std::runtime_error("bad escape");
        }
        r += s[p++];
    }
    if (p >= s.size()) throw std::runtime_error("unterminated string");
    ++p;
    return r;
}

uint16_t parse_port(const std::string& s, size_t& p) {
    size_t start = p;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
    if (p == start) throw std::runtime_error("expected integer");
    long v = std::stol(s.substr(start, p - start));
    if (v < 1 || v > 65535) throw std::runtime_error("port out of range");
    return static_cast<uint16_t>(v);
}

std::map<std::string, uint16_t> parse_buses(const std::string& s, size_t& p) {
    skip_ws(s, p);
    if (p >= s.size() || s[p] != '{') throw std::runtime_error("expected '{'");
    ++p;
    std::map<std::string, uint16_t> result;
    while (true) {
        skip_ws(s, p);
        if (p >= s.size()) throw std::runtime_error("unexpected EOF in buses");
        if (s[p] == '}') { ++p; break; }
        if (s[p] == ',') { ++p; continue; }
        auto key = parse_str(s, p);
        skip_ws(s, p);
        if (p >= s.size() || s[p] != ':') throw std::runtime_error("expected ':'");
        ++p;
        skip_ws(s, p);
        result[key] = parse_port(s, p);
    }
    return result;
}

// Skip any JSON value (for unknown keys at the root level).
void skip_value(const std::string& s, size_t& p) {
    skip_ws(s, p);
    if (p >= s.size()) return;
    if (s[p] == '"') { parse_str(s, p); return; }
    if (s[p] == '{' || s[p] == '[') {
        char open = s[p], close = (open == '{') ? '}' : ']';
        ++p;
        int depth = 1;
        while (p < s.size() && depth > 0) {
            if (s[p] == '"') { parse_str(s, p); continue; }
            if (s[p] == open)  ++depth;
            if (s[p] == close) --depth;
            ++p;
        }
        return;
    }
    // number / bool / null
    while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']') ++p;
}

std::map<std::string, uint16_t> parse_config_json(const std::string& s) {
    size_t p = 0;
    skip_ws(s, p);
    if (p >= s.size() || s[p] != '{') throw std::runtime_error("expected root object");
    ++p;
    while (p < s.size()) {
        skip_ws(s, p);
        if (p >= s.size()) break;
        if (s[p] == '}') break;
        if (s[p] == ',') { ++p; continue; }
        auto key = parse_str(s, p);
        skip_ws(s, p);
        if (p >= s.size() || s[p] != ':') throw std::runtime_error("expected ':'");
        ++p;
        skip_ws(s, p);
        if (key == "buses") return parse_buses(s, p);
        skip_value(s, p);
    }
    throw std::runtime_error("'buses' key not found");
}

// ---------------------------------------------------------------------------
// SHA-256 via OpenSSL EVP
// ---------------------------------------------------------------------------

std::string sha256_hex(const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);

    char hex[EVP_MAX_MD_SIZE * 2 + 1] = {};
    for (unsigned i = 0; i < dlen; ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return std::string(hex);
}

} // namespace

// ---------------------------------------------------------------------------

std::optional<AppConfig> load_config(
    const std::map<std::string, std::string>& iface_map,
    const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "config: cannot open %s: %s\n", path, strerror(errno));
        return std::nullopt;
    }
    std::string raw((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    std::map<std::string, uint16_t> bus_ports;
    try {
        bus_ports = parse_config_json(raw);
    } catch (const std::exception& e) {
        fprintf(stderr, "config: parse error: %s\n", e.what());
        return std::nullopt;
    }

    AppConfig cfg;
    cfg.sha256_hex = sha256_hex(raw);

    for (auto& [name, port] : bus_ports) {
        auto it = iface_map.find(name);
        if (it == iface_map.end()) {
            fprintf(stderr, "config: no --map for bus '%s', skipping\n", name.c_str());
            continue;
        }
        cfg.buses[name] = BusConfig{it->second, port};
    }

    if (cfg.buses.empty()) {
        fprintf(stderr, "config: no usable bus entries after interface mapping\n");
        return std::nullopt;
    }
    return cfg;
}

bool write_config(const char* path, const uint8_t* data, size_t len) {
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "config: cannot write %s: %s\n", tmp.c_str(), strerror(errno));
        return false;
    }
    bool ok = (fwrite(data, 1, len, f) == len);
    fclose(f);
    if (!ok) { remove(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path) != 0) {
        fprintf(stderr, "config: rename failed: %s\n", strerror(errno));
        remove(tmp.c_str());
        return false;
    }
    return true;
}