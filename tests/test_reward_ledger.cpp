// Quest rewards for everyone: the mirror/duplicate bookkeeping
// (include/QuestSync/RewardLedger.h).

#include "TestHarness.h"

#include "QuestSync/RewardLedger.h"

using namespace Pulsar::QuestSync;

namespace {
    constexpr std::uint32_t kRifle   = 0x0021C5A9u;
    constexpr std::uint32_t kCredits = 0x0000000Fu;
    constexpr std::uint64_t kHold    = RewardLedger::kHoldMs;
}

TEST(RewardLedger, PeerRewardIsGrantedAfterTheHold)
{
    RewardLedger l;
    l.Peer(kRifle, 1, 1000);
    CHECK_EQ(l.Due(1000 + kHold - 1).size(), 0u);
    const auto due = l.Due(1000 + kHold);
    CHECK_EQ(due.size(), 1u);
    CHECK_EQ(due[0].base, kRifle);
    CHECK_EQ(due[0].count, 1);
    CHECK_EQ(due[0].cancelled, 0);
    CHECK_EQ(l.Pending(), 0u);
    CHECK_EQ(l.Due(1000 + 2 * kHold).size(), 0u);   // once only
}

TEST(RewardLedger, OwnScriptCopyCancelsTheMirror)
{
    // Host got 500 credits; the client's own replicated stage script gives the
    // same 500 two seconds later. The client must end with 500, not 1000.
    RewardLedger client;
    client.Peer(kCredits, 500, 1000);
    const auto r = client.Local(kCredits, 500, 3000);
    CHECK_EQ(r.fresh, 500);       // sent to the host, which cancels it in turn
    CHECK_EQ(r.duplicate, 0);
    const auto due = client.Due(1000 + kHold);
    CHECK_EQ(due.size(), 1u);
    CHECK_EQ(due[0].count, 0);
    CHECK_EQ(due[0].cancelled, 500);
}

TEST(RewardLedger, EchoBackToTheOriginIsCancelled)
{
    // The host's own reward, then the client's script copy arrives as a peer reward.
    RewardLedger host;
    CHECK_EQ(host.Local(kRifle, 1, 1000).fresh, 1);
    host.Peer(kRifle, 1, 3500);
    const auto due = host.Due(3500 + kHold);
    CHECK_EQ(due.size(), 1u);
    CHECK_EQ(due[0].count, 0);
}

TEST(RewardLedger, LateScriptCopyAfterTheMirrorIsADuplicate)
{
    RewardLedger client;
    client.Peer(kRifle, 1, 1000);
    CHECK_EQ(client.Due(1000 + kHold)[0].count, 1);
    const auto r = client.Local(kRifle, 1, 1000 + kHold + 2000);
    CHECK_EQ(r.fresh, 0);
    CHECK_EQ(r.duplicate, 1);
}

TEST(RewardLedger, PartialOverlapGrantsTheRest)
{
    RewardLedger l;
    l.Local(kCredits, 200, 1000);
    l.Peer(kCredits, 500, 2000);
    const auto due = l.Due(2000 + kHold);
    CHECK_EQ(due.size(), 1u);
    CHECK_EQ(due[0].count, 300);
    CHECK_EQ(due[0].cancelled, 200);
}

TEST(RewardLedger, OldLocalRewardDoesNotCancel)
{
    // The same item earned long before is a different reward.
    RewardLedger l;
    l.Local(kRifle, 1, 1000);
    l.Peer(kRifle, 1, 1000 + RewardLedger::kWindowMs + 5000);
    CHECK_EQ(l.Due(1000 + RewardLedger::kWindowMs + 5000 + kHold)[0].count, 1);
}

TEST(RewardLedger, DifferentItemsDoNotCancel)
{
    RewardLedger l;
    l.Local(kCredits, 100, 1000);
    l.Peer(kRifle, 1, 1000);
    CHECK_EQ(l.Due(1000 + kHold)[0].count, 1);
    CHECK_EQ(l.Local(kCredits, 0, 1000).fresh, 0);
    CHECK_EQ(l.Local(0, 5, 1000).fresh, 0);
}
