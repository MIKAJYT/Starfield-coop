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
//
// 2-4 players: when a stage script rewards every player, each of them announces it, so
// the same reward arrives once per other player. Each sender's announcement is matched
// on its own against what the local player holds (its own reward, and mirrors granted for
// other senders), so the local player ends up with one copy, not one per sender.

#include <algorithm>
#include <array>
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

        // a_sender: the player ID of the peer that announced it (0-3).
        void Peer(std::uint32_t a_base, std::int32_t a_count, std::uint64_t a_nowMs, std::uint8_t a_sender = 1)
        {
            if (!a_base || a_count <= 0 || a_sender >= kSenders) return;
            Entry e{ a_base, a_count, a_nowMs };
            e.sender = a_sender;
            m_pending.push_back(e);
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
                std::int32_t cancelled = ConsumeFor(m_local, p.sender, p.base, p.count, p.ms);
                cancelled += ConsumeFor(m_granted, p.sender, p.base, p.count - cancelled, p.ms);
                const std::int32_t owed = p.count - cancelled;
                out.push_back({ p.base, owed, cancelled });
                if (owed > 0) {
                    Entry g{ p.base, owed, a_nowMs };
                    g.used[p.sender] = owed;   // a later reward from the same sender is a new one
                    m_granted.push_back(g);
                }
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
        static constexpr std::size_t kSenders = 4;

        struct Entry
        {
            std::uint32_t base;
            std::int32_t  count;
            std::uint64_t ms;
            std::uint8_t  sender = 1;                     // pending: who announced it
            std::array<std::int32_t, kSenders> used{};    // held: matched per sender so far
        };

        // Matches a sender's announcement against what the local player holds: each
        // held entry can cancel up to its count once per sender.
        static std::int32_t ConsumeFor(std::vector<Entry>& a_list, std::uint8_t a_sender, std::uint32_t a_base,
                                       std::int32_t a_count, std::uint64_t a_refMs)
        {
            std::int32_t taken = 0;
            for (auto& e : a_list) {
                if (taken >= a_count) break;
                if (e.base != a_base || e.count <= 0) continue;
                if (e.ms + kWindowMs < a_refMs) continue;
                const std::int32_t free = e.count - e.used[a_sender];
                if (free <= 0) continue;
                const std::int32_t t = (std::min)(free, a_count - taken);
                e.used[a_sender] += t;
                taken += t;
            }
            return taken;
        }

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
