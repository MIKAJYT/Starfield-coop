#pragma once

// Takeoff ready vote (owner's spec, phase 1: "if someone wants to fly away,
// there should be a voting/ready system. When all players accept, they get
// teleported to the ship and then it takes off"). The pilot's side of the vote,
// game-free so the unit tests can drive it (tests/test_takeoff_vote.cpp).
//
//   LaunchRequested() - the pilot asked the engine to take off. kHoldAndAsk: a
//                       new vote, hold the launch and ask the crew; kHold: a vote
//                       is already open, keep holding; kPass: let it through.
//   Ready(id, slot)   - a crew member is ready for vote id. kSendGo once every
//                       crew member is: the crew boards now.
//   Tick(now)         - kLaunch kBoardMs after the go; kTimedOut when the crew
//                       did not answer within kTimeoutMs.

#include <cstdint>
#include <set>

namespace Pulsar::Coop {

    class TakeoffVotePolicy
    {
    public:
        static constexpr std::uint64_t kTimeoutMs = 60000;
        static constexpr std::uint64_t kBoardMs   = 3000;

        enum class Request { kPass, kHoldAndAsk, kHold };
        enum class Answer { kNone, kSendGo };
        enum class Step { kNone, kLaunch, kTimedOut };

        Request LaunchRequested(std::uint64_t a_nowMs, int a_crew)
        {
            if (m_state == State::kBoarding) return Request::kHold;
            if (m_state == State::kWaiting) return Request::kHold;
            if (a_crew <= 0) return Request::kPass;
            m_state = State::kWaiting;
            ++m_id;
            m_crew = a_crew;
            m_ready.clear();
            m_sinceMs = a_nowMs;
            return Request::kHoldAndAsk;
        }

        Answer Ready(std::uint32_t a_id, int a_slot, std::uint64_t a_nowMs)
        {
            if (m_state != State::kWaiting || a_id != m_id) return Answer::kNone;
            m_ready.insert(a_slot);
            if (static_cast<int>(m_ready.size()) < m_crew) return Answer::kNone;
            m_state = State::kBoarding;
            m_sinceMs = a_nowMs;
            return Answer::kSendGo;
        }

        Step Tick(std::uint64_t a_nowMs)
        {
            if (m_state == State::kWaiting && a_nowMs - m_sinceMs >= kTimeoutMs) {
                m_state = State::kIdle;
                return Step::kTimedOut;
            }
            if (m_state == State::kBoarding && a_nowMs - m_sinceMs >= kBoardMs) {
                m_state = State::kIdle;
                return Step::kLaunch;
            }
            return Step::kNone;
        }

        // The pilot left the seat or the session ended.
        void Cancel() { m_state = State::kIdle; }

        std::uint32_t Id() const { return m_id; }
        bool          Open() const { return m_state != State::kIdle; }
        int           ReadyCount() const { return static_cast<int>(m_ready.size()); }
        int           Crew() const { return m_crew; }

    private:
        enum class State { kIdle, kWaiting, kBoarding };
        State         m_state = State::kIdle;
        std::uint32_t m_id = 0;
        int           m_crew = 0;
        std::set<int> m_ready;
        std::uint64_t m_sinceMs = 0;
    };

}  // namespace Pulsar::Coop
