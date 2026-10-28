#include "PCH.h"
#include "QuestSync/RewardShare.h"

#include "Cell3DAttach.h"
#include "Config.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "NPCSync/PluginLookup.h"
#include "QuestSync/RewardLedger.h"
#include "WorldSync/LootSync.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

using namespace std::string_view_literals;

namespace Pulsar::QuestSync::Reward {
namespace {

    struct LocalAdd
    {
        std::uint32_t base;
        std::int32_t  count;
        std::uint32_t reason;
        std::uint64_t ms;
    };

    std::mutex                 s_mutex;
    RewardLedger               s_ledger;     // guarded by s_mutex
    std::deque<LocalAdd>       s_localAdds;  // guarded by s_mutex
    std::atomic<std::uint32_t> s_peerRewards{ 0 };

    bool IsRewardReason(std::uint32_t a_reason)
    {
        return a_reason == static_cast<std::uint32_t>(RE::ITEM_TRANSFER_REASON::kQuestReward) ||
               a_reason == static_cast<std::uint32_t>(RE::ITEM_TRANSFER_REASON::kScriptAddItem);
    }

    void Send(std::uint32_t a_base, std::int32_t a_count, std::uint32_t a_reason)
    {
        using namespace Pulsar::Network;
        QuestRewardData d{};
        d.base    = a_base;
        d.localID = NPCSync::PluginLookup::FormIDToLocalID(a_base);
        NPCSync::PluginLookup::FormIDToPluginName(a_base, d.plugin, sizeof(d.plugin));
        d.count   = a_count;
        d.reason  = static_cast<std::uint8_t>(a_reason);
        std::uint8_t buf[1 + sizeof(QuestRewardData)];
        buf[0] = static_cast<std::uint8_t>(PKT_QUEST_REWARD);
        std::memcpy(buf + 1, &d, sizeof(d));
        Pulsar::Network::GetSingleton().Send(buf, sizeof(buf), /*reliable=*/true);
    }

    // Engine adds go through the vtable AddObjectToContainer; flagged as a
    // replay so LootSync's hook neither syncs nor re-captures it.
    void Grant(RE::PlayerCharacter* a_player, std::uint32_t a_base, std::int32_t a_count)
    {
        auto* form = RE::TESForm::LookupByID(a_base);
        if (!form || !form->IsBoundObject()) {
            PLOG_WARN("[PULSAR] [REWARD] partner's reward 0x{:08X} x{} not a local item - skipped"sv, a_base, a_count);
            return;
        }
        auto* bound = reinterpret_cast<RE::TESBoundObject*>(form);
        const bool prev = Pulsar::WorldSync::Loot::SetReplayingFromPacket(true);
        a_player->AddObjectToContainer(bound, nullptr, a_count, nullptr, RE::ITEM_TRANSFER_REASON::kNone);
        Pulsar::WorldSync::Loot::SetReplayingFromPacket(prev);
        PLOG_INFO("[PULSAR] [REWARD] +0x{:08X} x{} (partner's quest reward)"sv, a_base, a_count);
    }

}  // namespace

void OnLocalPlayerAdd(RE::TESForm* a_item, std::int32_t a_count, std::uint32_t a_reason)
{
    if (!a_item || a_count <= 0) return;
    const std::uint32_t base = a_item->GetFormID();
    if (!IsRewardReason(a_reason)) {
        // The first few per reason: which reasons the game uses is still being mapped.
        static std::atomic<std::uint8_t> s_seen[64]{};
        if (a_reason < 64 && s_seen[a_reason].fetch_add(1) < 3)
            PLOG_INFO("[PULSAR] [REWARD] player add 0x{:08X} x{} reason={} - not a reward"sv, base, a_count, a_reason);
        return;
    }
    if (a_item->GetFormType() == RE::FormType::kKEYM) return;   // key mirror
    if (!Pulsar::GetConfig().coopQuestRewards) return;
    if (!Pulsar::Network::GetSingleton().IsConnected()) return;
    std::lock_guard lk(s_mutex);
    s_localAdds.push_back({ base, a_count, a_reason, GetTickCount64() });
}

void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_length)
{
    using namespace Pulsar::Network;
    if (!a_data || a_length < 1 + sizeof(QuestRewardData)) return;
    QuestRewardData d{};
    std::memcpy(&d, a_data + 1, sizeof(d));
    d.plugin[sizeof(d.plugin) - 1] = '\0';
    if (!Pulsar::GetConfig().coopQuestRewards) {
        PLOG_INFO("[PULSAR] [REWARD] partner got 0x{:08X} x{} - not shared (CoopQuestRewards=0)"sv, d.base, d.count);
        return;
    }
    std::uint32_t resolved = 0;
    if (d.plugin[0] != '\0')
        resolved = NPCSync::PluginLookup::ResolveCrossPlugin(d.plugin, d.localID);
    if (!resolved) resolved = d.base;
    PLOG_INFO("[PULSAR] [REWARD] RX partner's reward 0x{:08X} (local 0x{:08X}) x{} reason={}"sv,
              d.base, resolved, d.count, d.reason);
    s_peerRewards.fetch_add(1);
    std::lock_guard lk(s_mutex);
    s_ledger.Peer(resolved, d.count, GetTickCount64());
}

void Tick()
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected()) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    const std::uint64_t now = GetTickCount64();

    struct Out { std::uint32_t base; std::int32_t fresh, duplicate; std::uint32_t reason; };
    std::vector<Out> local;
    std::vector<RewardGrant> due;
    {
        std::lock_guard lk(s_mutex);
        while (!s_localAdds.empty()) {
            const auto a = s_localAdds.front();
            s_localAdds.pop_front();
            const auto r = s_ledger.Local(a.base, a.count, a.ms);
            local.push_back({ a.base, r.fresh, r.duplicate, a.reason });
        }
        // Inventory adds wait out loading screens.
        if (!Pulsar::Cell3DAttach::IsQuiet())
            due = s_ledger.Due(now);
    }

    for (const auto& o : local) {
        if (o.duplicate > 0) {
            const auto removed = Pulsar::WorldSync::Loot::RawRemoveItems(player, o.base, o.duplicate);
            PLOG_INFO("[PULSAR] [REWARD] 0x{:08X} x{} copies the partner's reward we already granted - removed {}"sv,
                      o.base, o.duplicate, removed);
        }
        if (o.fresh > 0) {
            Send(o.base, o.fresh, o.reason);
            PLOG_INFO("[PULSAR] [REWARD] TX quest reward 0x{:08X} x{} reason={}"sv, o.base, o.fresh, o.reason);
        }
    }
    for (const auto& g : due) {
        if (g.cancelled > 0)
            PLOG_INFO("[PULSAR] [REWARD] partner's reward 0x{:08X}: x{} already given by our own quest"sv,
                      g.base, g.cancelled);
        if (g.count > 0) Grant(player, g.base, g.count);
    }
}

std::uint32_t PeerRewardsReceived()
{
    return s_peerRewards.load();
}

}  // namespace Pulsar::QuestSync::Reward
