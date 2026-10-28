#pragma once

// Quest rewards for everyone (owner's spec, phase 1): bookkeeping that decides
// how many of a partner's reward the local player still needs. Game-free so the
// unit tests can drive it (tests/test_reward_ledger.cpp).
//
// A quest reward can reach the second player twice: once through our own
// PKT_QUEST_REWARD mirror and once through the partner machine running the same
// stage script after QuestSync replicated the stage. The ledger cancels the two
// against each other by item and time:
//   - Local(base, n)  : a reward the local player just got from the game.
//                       Returns how much of it is new (to send to the peer); the
//                       rest duplicates a mirror we granted shortly before.
//   - Peer(base, n)   : a reward the peer got. Held kHoldMs so our own copy of
//                       the stage script has time to run first.
//   - Due()           : peer rewards whose hold ran out, minus what the local
//                       player already got of the same item in the window.
//                       Every expired one is returned, owed or not.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Pulsar::QuestSync {

    struct RewardGrant
    {
        std::uint32_t base;
        std::int32_t  count;       // still owed: grant it
        std::int32_t  cancelled;   // the local player already got this much
    };

    struct LocalRewardResult
    {
        std::int32_t fresh;       // send to the peer
        std::int32_t duplicate;   // copy of a mirror we already granted: take it back
    };

    class RewardLedger
    {
    public:
        static constexpr std::uint64_t kHoldMs   = 5000;
        static constexpr std::uint64_t kWindowMs = 30000;

        LocalRewardResult Local(std::uint32_t a_base, std::int32_t a_count, std::uint64_t a_nowMs)
        {
            Prune(a_nowMs);
            LocalRewardResult r{ 0, 0 };
            if (!a_base || a_count <= 0) return r;
            r.duplicate = Consume(m_granted, a_base, a_count, a_nowMs);
            r.fresh = a_count - r.duplicate;
            if (r.fresh > 0) m_local.push_back({ a_base, r.fresh, a_nowMs });
            return r;
        }

        void Peer(std::uint32_t a_base, std::int32_t a_count, std::uint64_t a_nowMs)
        {
            if (!a_base || a_count <= 0) return;
            m_pending.push_back({ a_base, a_count, a_nowMs });
        }

        std::vector<RewardGrant> Due(std::uint64_t a_nowMs)
        {
            std::vector<RewardGrant> out;
            std::vector<Entry> keep;
            for (const auto& p : m_pending) {
                if (a_nowMs < p.ms + kHoldMs) {
                    keep.push_back(p);
                    continue;
                }
                const std::int32_t cancelled = Consume(m_local, p.base, p.count, p.ms);
                const std::int32_t owed = p.count - cancelled;
                out.push_back({ p.base, owed, cancelled });
                if (owed > 0) m_granted.push_back({ p.base, owed, a_nowMs });
            }
            m_pending.swap(keep);
            Prune(a_nowMs);
            return out;
        }

        std::size_t Pending() const { return m_pending.size(); }

        void Clear()
        {
            m_local.clear();
            m_pending.clear();
            m_granted.clear();
        }

    private:
        struct Entry
        {
            std::uint32_t base;
            std::int32_t  count;
            std::uint64_t ms;
        };

        // Takes up to a_count of a_base from entries no older than kWindowMs
        // before a_refMs. Returns how much it took.
        static std::int32_t Consume(std::vector<Entry>& a_list, std::uint32_t a_base,
                                    std::int32_t a_count, std::uint64_t a_refMs)
        {
            std::int32_t taken = 0;
            for (auto& e : a_list) {
                if (taken >= a_count) break;
                if (e.base != a_base || e.count <= 0) continue;
                if (e.ms + kWindowMs < a_refMs) continue;
                const std::int32_t t = (std::min)(e.count, a_count - taken);
                e.count -= t;
                taken += t;
            }
            std::erase_if(a_list, [](const Entry& e) { return e.count <= 0; });
            return taken;
        }

        void Prune(std::uint64_t a_nowMs)
        {
            auto old = [a_nowMs](const Entry& e) { return e.ms + 2 * kWindowMs < a_nowMs; };
            std::erase_if(m_local, old);
            std::erase_if(m_granted, old);
        }

        std::vector<Entry> m_local;
        std::vector<Entry> m_pending;
        std::vector<Entry> m_granted;
    };

}  // namespace Pulsar::QuestSync
