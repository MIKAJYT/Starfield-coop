// 2-4 players: what the host forwards, and the forwarding and roster formats
// (include/Network/PeerRouting.h).

#include "TestHarness.h"

#include "Network/PeerRouting.h"

using namespace Pulsar::Network;
namespace R = Pulsar::Network::Routing;

TEST(PeerRouting, ControlPacketsStayBetweenOneClientAndTheHost)
{
    for (auto t : { PKT_HANDSHAKE, PKT_CRYPTO_HELLO, PKT_SAVE_TRANSFER, PKT_CLIENT_LOAD_COMPLETE,
                    PKT_CLIENT_PROFILE, PKT_TIME_WEATHER_REQ, PKT_NPC_DAMAGE, PKT_FROM, PKT_ROSTER })
        CHECK(!R::Forwarded(static_cast<std::uint8_t>(t)));
}

TEST(PeerRouting, PlayerAndWorldPacketsAreForwarded)
{
    for (auto t : { PKT_POSITION, PKT_BONES, PKT_EQUIP_V2, PKT_VOICE, PKT_TAKEOFF_VOTE, PKT_QUEST_REWARD, PKT_KILL_XP })
        CHECK(R::ClassOf(static_cast<std::uint8_t>(t)) == R::Class::kPlayer);
    for (auto t : { PKT_DOOR_STATE, PKT_CONTAINER_TAKE, PKT_QUEST_STAGE, PKT_LOCK_STATE, PKT_KEY_MIRROR })
        CHECK(R::ClassOf(static_cast<std::uint8_t>(t)) == R::Class::kWorld);
    CHECK(R::Forwarded(PKT_DOOR_STATE));
    CHECK(R::Forwarded(PKT_POSITION));
}

TEST(PeerRouting, WrapAndUnwrap)
{
    const std::uint8_t pos[] = { PKT_POSITION, 1, 2, 3 };
    const auto w = R::Wrap(2, pos, sizeof(pos));
    CHECK(w.size() == 6 && w[0] == PKT_FROM && w[1] == 2 && w[2] == PKT_POSITION);
    const auto u = R::Unwrap(w.data(), w.size());
    CHECK(u.sender == 2 && u.len == 4 && u.data[0] == PKT_POSITION && u.data[3] == 3);

    const auto plain = R::Unwrap(pos, sizeof(pos));
    CHECK(plain.sender == R::kHostId && plain.len == 4);
}

TEST(PeerRouting, BadWrappersAreRejected)
{
    const std::uint8_t tooShort[] = { PKT_FROM, 1 };
    CHECK(R::Unwrap(tooShort, sizeof(tooShort)).sender == R::kNoPlayer);
    const std::uint8_t fromHost[] = { PKT_FROM, 0, PKT_POSITION };       // the host never needs a wrapper
    CHECK(R::Unwrap(fromHost, sizeof(fromHost)).sender == R::kNoPlayer);
    const std::uint8_t badId[] = { PKT_FROM, 7, PKT_POSITION };
    CHECK(R::Unwrap(badId, sizeof(badId)).sender == R::kNoPlayer);
    const std::uint8_t nested[] = { PKT_FROM, 1, PKT_FROM, 2, PKT_POSITION };
    CHECK(R::Unwrap(nested, sizeof(nested)).sender == R::kNoPlayer);
}

TEST(PeerRouting, Roster)
{
    const auto b = R::MakeRoster(2, { 0, 1, 2 });
    R::Roster r;
    CHECK(R::ParseRoster(b.data(), b.size(), r));
    CHECK(r.yourId == 2);
    CHECK(r.ids.size() == 3 && r.ids[0] == 0 && r.ids[2] == 2);

    auto bad = b;
    bad[1] = 0;                                   // a client is never player 0
    CHECK(!R::ParseRoster(bad.data(), bad.size(), r));
    bad = b;
    bad.push_back(3);                             // count does not match
    CHECK(!R::ParseRoster(bad.data(), bad.size(), r));
}
