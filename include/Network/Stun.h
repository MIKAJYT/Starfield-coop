#pragma once

// Minimal STUN (RFC 5389) Binding client messages, game-free (docs/SPEC-STATUS.md, phase 2).
// The plugin sends a Binding request from ENet's own UDP socket and catches the response in
// ENet's intercept callback, so the public address it learns is the NAT mapping of the
// socket the game traffic uses. A STUN server only reports that address; it never carries
// game traffic.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Pulsar::Network::Stun {

    inline constexpr std::uint32_t kMagicCookie = 0x2112A442;
    inline constexpr std::size_t   kHeaderBytes = 20;
    using TransactionId = std::array<std::uint8_t, 12>;

    struct Mapped { std::uint32_t ipv4; std::uint16_t port; };   // ipv4 network order, port host order

    // A 20-byte Binding request with no attributes.
    inline std::array<std::uint8_t, kHeaderBytes> MakeBindingRequest(const TransactionId& a_tx)
    {
        std::array<std::uint8_t, kHeaderBytes> m{};
        m[0] = 0x00; m[1] = 0x01;   // Binding request
        m[2] = 0x00; m[3] = 0x00;   // length
        m[4] = 0x21; m[5] = 0x12; m[6] = 0xA4; m[7] = 0x42;
        for (std::size_t i = 0; i < 12; ++i) m[8 + i] = a_tx[i];
        return m;
    }

    // True if the datagram looks like a STUN message (cheap check for the intercept).
    inline bool LooksLikeStun(const std::uint8_t* a_d, std::size_t a_n)
    {
        return a_n >= kHeaderBytes && (a_d[0] & 0xC0) == 0 && a_d[4] == 0x21 && a_d[5] == 0x12 &&
               a_d[6] == 0xA4 && a_d[7] == 0x42 && ((a_d[2] << 8) | a_d[3]) + kHeaderBytes == a_n;
    }

    // The mapped IPv4 address of a Binding success response with our transaction ID:
    // XOR-MAPPED-ADDRESS (0x0020) preferred, MAPPED-ADDRESS (0x0001) accepted.
    inline std::optional<Mapped> ParseBindingResponse(const std::uint8_t* a_d, std::size_t a_n, const TransactionId& a_tx)
    {
        if (!LooksLikeStun(a_d, a_n)) return std::nullopt;
        if (a_d[0] != 0x01 || a_d[1] != 0x01) return std::nullopt;   // Binding success response
        for (std::size_t i = 0; i < 12; ++i) if (a_d[8 + i] != a_tx[i]) return std::nullopt;
        std::optional<Mapped> plain;
        std::size_t p = kHeaderBytes;
        while (p + 4 <= a_n) {
            const std::uint16_t type = static_cast<std::uint16_t>((a_d[p] << 8) | a_d[p + 1]);
            const std::uint16_t len  = static_cast<std::uint16_t>((a_d[p + 2] << 8) | a_d[p + 3]);
            const std::size_t   v    = p + 4;
            if (v + len > a_n) return std::nullopt;
            if ((type == 0x0020 || type == 0x0001) && len >= 8 && a_d[v + 1] == 0x01) {   // family IPv4
                std::uint16_t port = static_cast<std::uint16_t>((a_d[v + 2] << 8) | a_d[v + 3]);
                std::uint8_t  ip[4] = { a_d[v + 4], a_d[v + 5], a_d[v + 6], a_d[v + 7] };
                if (type == 0x0020) {
                    port ^= 0x2112;
                    ip[0] ^= 0x21; ip[1] ^= 0x12; ip[2] ^= 0xA4; ip[3] ^= 0x42;
                }
                const Mapped m{ static_cast<std::uint32_t>(ip[0]) | (static_cast<std::uint32_t>(ip[1]) << 8) |
                                (static_cast<std::uint32_t>(ip[2]) << 16) | (static_cast<std::uint32_t>(ip[3]) << 24),
                                port };
                if (type == 0x0020) return m;
                plain = m;
            }
            p = v + ((len + 3u) & ~3u);   // attributes are padded to 4 bytes
        }
        return plain;
    }

}  // namespace Pulsar::Network::Stun
