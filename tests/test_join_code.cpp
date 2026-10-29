// Join codes and STUN parsing for direct sessions (include/Network/JoinCode.h, Stun.h).

#include "TestHarness.h"

#include "Network/JoinCode.h"
#include "Network/Stun.h"

#include <cstring>

namespace JC = Pulsar::Network::JoinCode;
namespace Stun = Pulsar::Network::Stun;

namespace {
    std::uint32_t Ip(int a, int b, int c, int d)   // network order, as ENetAddress::host
    {
        return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) |
               (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
    }

    JC::Code SampleHost()
    {
        JC::Code c;
        c.kind = JC::Kind::kHost;
        for (std::size_t i = 0; i < c.secret.size(); ++i) c.secret[i] = static_cast<std::uint8_t>(0xA0 + i);
        c.endpoints = { { JC::Via::kLan, Ip(192, 168, 1, 20), 7777 },
                        { JC::Via::kPublic, Ip(203, 0, 113, 7), 51234 },
                        { JC::Via::kMapped, Ip(203, 0, 113, 7), 7777 } };
        return c;
    }
}

TEST(JoinCode, Crc16KnownValue)
{
    const char* s = "123456789";
    CHECK(JC::Crc16(reinterpret_cast<const std::uint8_t*>(s), 9) == 0x29B1);
}

TEST(JoinCode, HostCodeRoundTrip)
{
    const auto c = SampleHost();
    const auto text = JC::Encode(c);
    CHECK(text.rfind("PSR-", 0) == 0);
    // Pinned: the launcher's C# reader (lobby/pulsar/Services/JoinCodeText.cs) is tested
    // against this same text (lobby/companion.tests/JoinCodeTextTests.cs).
    CHECK(text == "PSR-040T1-8D2ME-JAB9N-7N2MT-NAXCN-PQAY0-R1R2M-0250Y-C41CP-03H0Z-4240Y-B01RG-E7K1J-BA0");
    const auto d = JC::Decode(text);
    CHECK(d.has_value());
    CHECK(d->kind == JC::Kind::kHost);
    CHECK(d->secret == c.secret);
    CHECK(d->endpoints == c.endpoints);
}

TEST(JoinCode, ReplyCodeRoundTrip)
{
    JC::Code r;
    r.kind = JC::Kind::kReply;
    r.tag = JC::TagOf(SampleHost().secret);
    r.endpoints = { { JC::Via::kPublic, Ip(198, 51, 100, 9), 60001 } };
    const auto d = JC::Decode(JC::Encode(r));
    CHECK(d.has_value());
    CHECK(d->kind == JC::Kind::kReply);
    CHECK(d->tag == JC::TagOf(SampleHost().secret));
    CHECK(d->endpoints == r.endpoints);
}

TEST(JoinCode, ForgivingInput)
{
    auto text = JC::Encode(SampleHost());
    std::string sloppy;
    for (char ch : text) {
        if (ch == '-') { sloppy += "  "; continue; }
        if (ch == '0') { sloppy.push_back('o'); continue; }   // O for zero, lower case
        if (ch == '1') { sloppy.push_back('l'); continue; }   // l for one
        sloppy.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    }
    sloppy += "\r\n";
    const auto d = JC::Decode(sloppy);
    CHECK(d.has_value());
    CHECK(d->endpoints == SampleHost().endpoints);
}

TEST(JoinCode, TypoIsRejected)
{
    const auto text = JC::Encode(SampleHost());
    int rejected = 0, tried = 0;
    for (std::size_t i = 4; i < text.size(); ++i) {
        if (text[i] == '-') continue;
        std::string t = text;
        t[i] = (t[i] == 'X') ? 'Y' : 'X';   // one wrong character
        ++tried;
        if (!JC::Decode(t)) ++rejected;
    }
    CHECK(tried > 20);
    CHECK(rejected == tried);
}

TEST(JoinCode, GarbageIsRejected)
{
    CHECK(!JC::Decode(""));
    CHECK(!JC::Decode("PSR-"));
    CHECK(!JC::Decode("hello world"));
    CHECK(!JC::Decode("PSR-UUUUU"));   // U is not in the alphabet
    auto b = JC::ToBytes(SampleHost());
    b[0] = 9;                           // unknown version, CRC fixed up
    const auto crc = JC::Crc16(b.data(), b.size() - 2);
    b[b.size() - 2] = static_cast<std::uint8_t>(crc >> 8);
    b[b.size() - 1] = static_cast<std::uint8_t>(crc);
    CHECK(!JC::FromBytes(b));
}

TEST(JoinCode, EndpointCountIsCapped)
{
    auto c = SampleHost();
    while (c.endpoints.size() < 10) c.endpoints.push_back({ JC::Via::kLan, Ip(10, 0, 0, 1), 7777 });
    const auto d = JC::Decode(JC::Encode(c));
    CHECK(d.has_value());
    CHECK(d->endpoints.size() == JC::kMaxEndpoints);
}

// RFC 5769, section 2.2: sample IPv4 Binding response (XOR-MAPPED-ADDRESS 192.0.2.1:32853).
TEST(Stun, Rfc5769Ipv4Response)
{
    const std::uint8_t msg[] = {
        0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01,
        0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
        0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63,
        0x74, 0x6f, 0x72, 0x20,
        0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
        0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0x19,
        0x38, 0xdf, 0x7c, 0x0e, 0x4b, 0x4a, 0x7f, 0xb4, 0x81, 0xe4, 0xe9, 0xf6,
        0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96,
    };
    const Stun::TransactionId tx = { 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae };
    CHECK(Stun::LooksLikeStun(msg, sizeof(msg)));
    const auto m = Stun::ParseBindingResponse(msg, sizeof(msg), tx);
    CHECK(m.has_value());
    CHECK(m->ipv4 == Ip(192, 0, 2, 1));
    CHECK(m->port == 32853);

    Stun::TransactionId other = tx;
    other[11] ^= 1;
    CHECK(!Stun::ParseBindingResponse(msg, sizeof(msg), other));   // not our request
    CHECK(!Stun::ParseBindingResponse(msg, sizeof(msg) - 4, tx));  // truncated
}

TEST(Stun, RequestShape)
{
    const Stun::TransactionId tx = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    const auto r = Stun::MakeBindingRequest(tx);
    CHECK(r[0] == 0 && r[1] == 1 && r[2] == 0 && r[3] == 0);
    CHECK(Stun::LooksLikeStun(r.data(), r.size()));
    CHECK(std::memcmp(r.data() + 8, tx.data(), 12) == 0);
    const std::uint8_t enetLike[24] = { 0x80, 0x00, 0x12, 0x34 };   // an ENet datagram is not STUN
    CHECK(!Stun::LooksLikeStun(enetLike, sizeof(enetLike)));
}
