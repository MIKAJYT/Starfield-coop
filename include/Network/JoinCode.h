#pragma once

// Join codes for direct sessions without a relay or lobby server (docs/SPEC-STATUS.md,
// phase 2). Game-free.
//
// A host code carries the session secret and the host's reachable endpoints; the friend
// types it in. If the friend can't reach the host with it alone, the friend's game makes a
// reply code (its own public endpoints, tagged with the session) that the host types in,
// so both sides punch toward each other.
//
// Text form: "PSR-" + Crockford base32 in groups of 5, e.g. "PSR-0A1B2-C3D4E-...".
// Decoding ignores case, dashes and spaces, reads O as 0 and I/L as 1, and rejects any
// code whose CRC-16 does not match (a typo).
//
// Binary form: [version 1][kind 1][secret 16 (host) | session tag 4 (reply)][count 1]
//              count x [endpoint kind 1][IPv4 4, network order][port 2, big endian]
//              [CRC-16/CCITT-FALSE 2, big endian, over everything before it]

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pulsar::Network::JoinCode {

    inline constexpr std::uint8_t kVersion      = 1;
    inline constexpr std::size_t  kSecretBytes  = 16;
    inline constexpr std::size_t  kTagBytes     = 4;
    inline constexpr std::size_t  kMaxEndpoints = 6;

    enum class Kind : std::uint8_t { kHost = 1, kReply = 2 };

    enum class Via : std::uint8_t {
        kLan    = 1,   // a local network address of the PC
        kPublic = 2,   // the public address a STUN server saw (NAT mapping of the game socket)
        kMapped = 3,   // a port the router opened by UPnP
    };

    struct Endpoint
    {
        Via           via = Via::kLan;
        std::uint32_t ipv4 = 0;   // network byte order, as ENetAddress::host
        std::uint16_t port = 0;   // host byte order, as ENetAddress::port

        bool operator==(const Endpoint&) const = default;
    };

    struct Code
    {
        Kind                                   kind = Kind::kHost;
        std::array<std::uint8_t, kSecretBytes> secret{};   // host codes only
        std::array<std::uint8_t, kTagBytes>    tag{};      // reply codes only: secret[0..3]
        std::vector<Endpoint>                  endpoints;
    };

    // CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). "123456789" -> 0x29B1.
    inline std::uint16_t Crc16(const std::uint8_t* a_data, std::size_t a_len)
    {
        std::uint16_t crc = 0xFFFF;
        for (std::size_t i = 0; i < a_len; ++i) {
            crc ^= static_cast<std::uint16_t>(a_data[i]) << 8;
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
        }
        return crc;
    }

    inline constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford

    // The reply tag that binds a reply code to a session.
    inline std::array<std::uint8_t, kTagBytes> TagOf(const std::array<std::uint8_t, kSecretBytes>& a_secret)
    {
        return { a_secret[0], a_secret[1], a_secret[2], a_secret[3] };
    }

    inline std::vector<std::uint8_t> ToBytes(const Code& a_code)
    {
        std::vector<std::uint8_t> b;
        b.push_back(kVersion);
        b.push_back(static_cast<std::uint8_t>(a_code.kind));
        if (a_code.kind == Kind::kHost) b.insert(b.end(), a_code.secret.begin(), a_code.secret.end());
        else                            b.insert(b.end(), a_code.tag.begin(), a_code.tag.end());
        const std::size_t n = a_code.endpoints.size() < kMaxEndpoints ? a_code.endpoints.size() : kMaxEndpoints;
        b.push_back(static_cast<std::uint8_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = a_code.endpoints[i];
            b.push_back(static_cast<std::uint8_t>(e.via));
            const std::uint32_t ip = e.ipv4;   // network order: first octet in the low byte
            b.push_back(static_cast<std::uint8_t>(ip));
            b.push_back(static_cast<std::uint8_t>(ip >> 8));
            b.push_back(static_cast<std::uint8_t>(ip >> 16));
            b.push_back(static_cast<std::uint8_t>(ip >> 24));
            b.push_back(static_cast<std::uint8_t>(e.port >> 8));
            b.push_back(static_cast<std::uint8_t>(e.port));
        }
        const std::uint16_t crc = Crc16(b.data(), b.size());
        b.push_back(static_cast<std::uint8_t>(crc >> 8));
        b.push_back(static_cast<std::uint8_t>(crc));
        return b;
    }

    inline std::optional<Code> FromBytes(const std::vector<std::uint8_t>& a_b)
    {
        if (a_b.size() < 2 + kTagBytes + 1 + 2) return std::nullopt;
        const std::uint16_t crc = static_cast<std::uint16_t>((a_b[a_b.size() - 2] << 8) | a_b[a_b.size() - 1]);
        if (Crc16(a_b.data(), a_b.size() - 2) != crc) return std::nullopt;
        if (a_b[0] != kVersion) return std::nullopt;
        Code c;
        std::size_t p = 1;
        const auto kind = a_b[p++];
        if (kind != 1 && kind != 2) return std::nullopt;
        c.kind = static_cast<Kind>(kind);
        const std::size_t idBytes = c.kind == Kind::kHost ? kSecretBytes : kTagBytes;
        if (a_b.size() < p + idBytes + 1 + 2) return std::nullopt;
        for (std::size_t i = 0; i < idBytes; ++i) {
            if (c.kind == Kind::kHost) c.secret[i] = a_b[p + i];
            else                       c.tag[i] = a_b[p + i];
        }
        p += idBytes;
        const std::size_t n = a_b[p++];
        if (n > kMaxEndpoints || a_b.size() != p + n * 7 + 2) return std::nullopt;
        for (std::size_t i = 0; i < n; ++i, p += 7) {
            const auto via = a_b[p];
            if (via < 1 || via > 3) return std::nullopt;
            Endpoint e;
            e.via  = static_cast<Via>(via);
            e.ipv4 = static_cast<std::uint32_t>(a_b[p + 1]) | (static_cast<std::uint32_t>(a_b[p + 2]) << 8) |
                     (static_cast<std::uint32_t>(a_b[p + 3]) << 16) | (static_cast<std::uint32_t>(a_b[p + 4]) << 24);
            e.port = static_cast<std::uint16_t>((a_b[p + 5] << 8) | a_b[p + 6]);
            c.endpoints.push_back(e);
        }
        return c;
    }

    inline std::string Encode(const Code& a_code)
    {
        const auto b = ToBytes(a_code);
        std::string raw;
        std::uint32_t acc = 0;
        int bits = 0;
        for (auto byte : b) {
            acc = (acc << 8) | byte;
            bits += 8;
            while (bits >= 5) {
                raw.push_back(kAlphabet[(acc >> (bits - 5)) & 31]);
                bits -= 5;
            }
        }
        if (bits > 0) raw.push_back(kAlphabet[(acc << (5 - bits)) & 31]);
        std::string out = "PSR";
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (i % 5 == 0) out.push_back('-');
            out.push_back(raw[i]);
        }
        return out;
    }

    inline std::optional<Code> Decode(std::string_view a_text)
    {
        std::string s;
        for (char ch : a_text) {
            if (ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            s.push_back(ch);
        }
        if (s.rfind("PSR", 0) != 0) return std::nullopt;
        s.erase(0, 3);
        std::vector<std::uint8_t> b;
        std::uint32_t acc = 0;
        int bits = 0;
        for (char ch : s) {
            if (ch == 'O') ch = '0';
            if (ch == 'I' || ch == 'L') ch = '1';
            int v = -1;
            for (int i = 0; i < 32; ++i) if (kAlphabet[i] == ch) { v = i; break; }
            if (v < 0) return std::nullopt;
            acc = (acc << 5) | static_cast<std::uint32_t>(v);
            bits += 5;
            if (bits >= 8) {
                b.push_back(static_cast<std::uint8_t>(acc >> (bits - 8)));
                bits -= 8;
            }
        }
        return FromBytes(b);
    }

}  // namespace Pulsar::Network::JoinCode
