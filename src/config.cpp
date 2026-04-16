#include "config.hpp"

#include <openssl/evp.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// SHA-256 helpers
// ---------------------------------------------------------------------------

std::string sha256_hex_of(const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  dlen = 0;
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);

    char hex[EVP_MAX_MD_SIZE * 2 + 1] = {};
    for (unsigned i = 0; i < dlen; ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return std::string(hex);
}

std::string sha256_hex_of_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string raw((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    return sha256_hex_of(raw);
}

// ---------------------------------------------------------------------------
// JSON serialiser — produces the on-disk port_assignments.json
// ---------------------------------------------------------------------------

namespace {

std::string json_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == '"')  { r += "\\\""; }
        else if (c == '\\') { r += "\\\\"; }
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            r += buf;
        } else {
            r += static_cast<char>(c);
        }
    }
    return r;
}

std::string serialise(const StoredConfig& cfg) {
    std::string s;
    s  = "{\n  \"assigned_id\": ";
    s += std::to_string(cfg.assigned_id);
    s += ",\n  \"ports\": [\n";

    for (size_t i = 0; i < cfg.ports.size(); ++i) {
        const auto& p = cfg.ports[i];
        s += "    {\n";
        s += "      \"port_id\": \""     + json_escape(p.port_id)      + "\",\n";
        s += "      \"bus_name\": \""    + json_escape(p.bus_name)     + "\",\n";
        s += "      \"description\": \"" + json_escape(p.description)  + "\",\n";
        s += "      \"bitrate\": "       + std::to_string(p.bitrate)   + ",\n";
        s += "      \"udp_port\": "      + std::to_string(p.udp_port)  + ",\n";
        s += "      \"enabled\": "       + (p.enabled ? "true" : "false") + "\n";
        s += "    }";
        if (i + 1 < cfg.ports.size()) s += ",";
        s += "\n";
    }
    s += "  ]\n}\n";
    return s;
}

// ---------------------------------------------------------------------------
// Minimal recursive-descent JSON parser for the port_assignments format
// ---------------------------------------------------------------------------

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
            switch (s[p]) {
            case '"': case '\\': case '/': r += s[p]; break;
            case 'n': r += '\n'; break;
            case 'r': r += '\r'; break;
            case 't': r += '\t'; break;
            default:  r += s[p]; break;
            }
        } else {
            r += s[p];
        }
        ++p;
    }
    if (p >= s.size()) throw std::runtime_error("unterminated string");
    ++p;
    return r;
}

long long parse_int(const std::string& s, size_t& p) {
    size_t start = p;
    if (p < s.size() && s[p] == '-') ++p;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
    if (p == start) throw std::runtime_error("expected integer");
    return std::stoll(s.substr(start, p - start));
}

bool parse_bool(const std::string& s, size_t& p) {
    if (s.substr(p, 4) == "true")  { p += 4; return true; }
    if (s.substr(p, 5) == "false") { p += 5; return false; }
    throw std::runtime_error("expected true/false");
}

void expect_char(const std::string& s, size_t& p, char c) {
    skip_ws(s, p);
    if (p >= s.size() || s[p] != c)
        throw std::runtime_error(std::string("expected '") + c + "'");
    ++p;
}

PortAssignment parse_port_object(const std::string& s, size_t& p) {
    expect_char(s, p, '{');
    PortAssignment pa;
    while (true) {
        skip_ws(s, p);
        if (p >= s.size()) throw std::runtime_error("unexpected EOF in port object");
        if (s[p] == '}') { ++p; break; }
        if (s[p] == ',') { ++p; continue; }

        auto key = parse_str(s, p);
        expect_char(s, p, ':');
        skip_ws(s, p);

        if      (key == "port_id")     pa.port_id      = parse_str(s, p);
        else if (key == "bus_name")    pa.bus_name     = parse_str(s, p);
        else if (key == "description") pa.description  = parse_str(s, p);
        else if (key == "bitrate")     pa.bitrate      = static_cast<uint32_t>(parse_int(s, p));
        else if (key == "udp_port")    pa.udp_port     = static_cast<uint16_t>(parse_int(s, p));
        else if (key == "enabled")     pa.enabled      = parse_bool(s, p);
        else {
            // Skip unknown value (string or number or bool)
            if (p < s.size() && s[p] == '"') parse_str(s, p);
            else {
                while (p < s.size() && s[p] != ',' && s[p] != '}') ++p;
            }
        }
    }
    return pa;
}

StoredConfig parse_stored_config(const std::string& s) {
    size_t p = 0;
    StoredConfig cfg;
    expect_char(s, p, '{');

    while (true) {
        skip_ws(s, p);
        if (p >= s.size()) break;
        if (s[p] == '}') break;
        if (s[p] == ',') { ++p; continue; }

        auto key = parse_str(s, p);
        expect_char(s, p, ':');
        skip_ws(s, p);

        if (key == "assigned_id") {
            cfg.assigned_id = static_cast<uint32_t>(parse_int(s, p));
        } else if (key == "ports") {
            expect_char(s, p, '[');
            while (true) {
                skip_ws(s, p);
                if (p >= s.size()) break;
                if (s[p] == ']') { ++p; break; }
                if (s[p] == ',') { ++p; continue; }
                cfg.ports.push_back(parse_port_object(s, p));
            }
        } else {
            // skip unknown top-level value
            while (p < s.size() && s[p] != ',' && s[p] != '}') ++p;
        }
    }
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------

std::optional<StoredConfig> load_stored_config(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt; // file absent = virgin device

    std::string raw((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    try {
        StoredConfig cfg = parse_stored_config(raw);
        cfg.sha256_hex   = sha256_hex_of(raw);
        return cfg;
    } catch (const std::exception& e) {
        fprintf(stderr, "config: malformed %s: %s — treating as virgin\n",
                path, e.what());
        return std::nullopt;
    }
}

bool save_stored_config(StoredConfig& cfg, const char* path) {
    std::string json = serialise(cfg);
    cfg.sha256_hex   = sha256_hex_of(json);

    std::string tmp  = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "config: cannot write %s: %s\n", tmp.c_str(), strerror(errno));
        return false;
    }
    bool ok = (fwrite(json.data(), 1, json.size(), f) == json.size());
    fclose(f);
    if (!ok) { remove(tmp.c_str()); return false; }

    if (rename(tmp.c_str(), path) != 0) {
        fprintf(stderr, "config: rename failed: %s\n", strerror(errno));
        remove(tmp.c_str());
        return false;
    }
    return true;
}
