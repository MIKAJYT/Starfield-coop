#pragma once

#include <cstddef>
#include <cstdint>

namespace RE { class TESForm; }

namespace Pulsar::QuestSync::Reward {

    // Quest rewards for everyone (owner's spec, phase 1; CoopQuestRewards).
    //
    // Capture: LootSync's AddObjectToContainer hook calls OnLocalPlayerAdd for
    // every add to the local player that has no source container. Adds with
    // reason kQuestReward or kScriptAddItem count as rewards (keys excluded:
    // the key mirror covers them). Tick sends them to the peer.
    //
    // Apply: OnPacketReceived queues the peer's reward; Tick grants it after the
    // ledger's hold, minus what the local player's own copy of the quest already
    // gave (RewardLedger.h). A local reward that copies a mirror granted shortly
    // before is taken back and not sent.
    void OnLocalPlayerAdd(RE::TESForm* a_item, std::int32_t a_count, std::uint32_t a_reason);
    void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_length);
    void Tick();   // game thread

    // Test bot: peer rewards received this session.
    std::uint32_t PeerRewardsReceived();

}  // namespace Pulsar::QuestSync::Reward
