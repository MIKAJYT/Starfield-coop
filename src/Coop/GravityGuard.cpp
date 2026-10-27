// Gravity guard - known bug "the client's gravity can stay disabled after a
// station or space transition".
//
// Theory (2026-10-27, unverified in-game): co-location on the client does NOT
// walk through the load door like vanilla play. It engine-MoveTo's the player
// onto the host clone and then SetPosition's it (Main.cpp TELE-DIAG path,
// TransitSync::SafeSetPlayerPos). Vanilla zero-G -> gravity transitions run
// ActionGravityOn (AACT 0x24655) on the player; if the teleport skips that
// step the player keeps floating in a cell that has gravity.
//
// This module:
//   1. logs the local player's gravity state (GRAV:) on every change and
//      ~2.5s after every transition, plus every engine ActionGravityOn/Off on
//      the local player - the evidence for confirming / refuting the theory;
//   2. client + coopGravityAutoFix: after a transition that went zero-G ->
//      gravity WITHOUT the engine firing ActionGravityOn, fires it once;
//   3. hotkeyGravityFix (default F4): fires ActionGravityOn on demand and
//      logs the state before and ~1s after.

#include "PCH.h"
#include "Coop/GravityGuard.h"
#include "Network/NetworkHost.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "NASLog.h"
#include "Config.h"

#include <atomic>

#include <RE/B/BSFixedString.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/U/UI.h>

#include <windows.h>

using namespace std::string_view_literals;

namespace Pulsar::Coop::GravityGuard
{
    namespace
    {
        // Settle time after a transition before we judge the gravity state.
        // Cell attach + 3D load finish well inside this on 1.16.244 (CellDiag
        // uses 3000ms for the heavier clone-3D check).
        constexpr std::uint64_t kSettleMs        = 2500;
        // Re-snapshot after a repair so the log shows whether it took.
        constexpr std::uint64_t kVerifyMs        = 1000;
        // Rate limit for change-driven GRAV: lines.
        constexpr std::uint64_t kChangeLogMinMs  = 250;

        struct GravSnap {
            std::uint32_t cellID;
            bool          planetGravityCell;
            bool          inSpace;
            bool          zeroG;
            float         gravityScale;
            float         playerGravity;
            bool          valid;
        };

        // SEH-wrapped reads. IsInZeroGravity / GetGravityScale are virtuals on a
        // player that may be mid cell-attach. No C++ unwinding objects (C2712).
        GravSnap RawSnapGravity(RE::PlayerCharacter* a_player) noexcept
        {
            GravSnap g{};
            __try {
                if (!a_player) return g;
                if (auto* cell = a_player->parentCell) {
                    g.cellID            = cell->formID;
                    g.planetGravityCell = cell->UsesPlanetGravity();
                }
                g.inSpace       = a_player->IsInSpace(true);
                g.zeroG         = a_player->IsInZeroGravity();
                g.gravityScale  = a_player->GetGravityScale();
                g.playerGravity = a_player->playerGravity;
                g.valid         = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return g;
        }

        bool RawCheckMenuOpen(RE::UI* a_ui, const RE::BSFixedString& a_name) noexcept
        {
            __try {
                return a_ui && a_ui->IsMenuOpen(a_name);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool Differs(const GravSnap& a, const GravSnap& b)
        {
            auto ne = [](float x, float y) { return x - y > 0.01f || y - x > 0.01f; };
            return a.cellID != b.cellID || a.planetGravityCell != b.planetGravityCell ||
                   a.inSpace != b.inSpace || a.zeroG != b.zeroG ||
                   ne(a.gravityScale, b.gravityScale) || ne(a.playerGravity, b.playerGravity);
        }

        void LogSnap(const char* a_tag, const char* a_why, const GravSnap& g)
        {
            PLOG_INFO(
                "[PULSAR] GRAV: {} {} cell=0x{:08X} planetGravCell={} inSpace={} zeroG={} "
                "gravScale={:.3f} playerGravity={:.3f}"sv,
                a_tag, a_why, g.cellID, g.planetGravityCell ? 1 : 0, g.inSpace ? 1 : 0,
                g.zeroG ? 1 : 0, g.gravityScale, g.playerGravity);
            NASLOG("GRAV %s %s cell=0x%08X pgc=%d space=%d zeroG=%d scale=%.3f pg=%.3f",
                   a_tag, a_why, g.cellID, g.planetGravityCell ? 1 : 0, g.inSpace ? 1 : 0,
                   g.zeroG ? 1 : 0, g.gravityScale, g.playerGravity);
        }

        struct State {
            GravSnap      last{};
            std::uint64_t lastChangeLog   = 0;
            bool          lastInLoadMenu  = false;
            // Transition tracking: armed on cell-change / loadMenu CLOSED.
            std::uint64_t checkDueAt      = 0;
            bool          zeroGAtStart    = false;
            std::uint32_t gravOnSeqAtStart = 0;
            std::uint64_t verifyDueAt     = 0;
            bool          hotkeyDown      = false;
        };
        State s_state;

        // Bumped by the PerformAction hook (any thread).
        std::atomic<std::uint32_t> s_gravOnSeq{ 0 };
        std::atomic<std::uint32_t> s_gravOffSeq{ 0 };

        bool FireGravityOn(RE::PlayerCharacter* a_player)
        {
            return Pulsar::RemotePlayer::PerformActionOnActor(a_player, kActionGravityOn);
        }

        void ArmTransitionCheck(State& s, std::uint64_t a_now)
        {
            // Keep the zero-G state from BEFORE the first leg of a multi-step
            // transition (cell-change then loadMenu close re-arm within the window).
            if (s.checkDueAt == 0) {
                s.zeroGAtStart     = s.last.zeroG;
                s.gravOnSeqAtStart = s_gravOnSeq.load(std::memory_order_relaxed);
            }
            s.checkDueAt = a_now + kSettleMs;
        }
    }

    void OnLocalGravityAction(bool a_on)
    {
        (a_on ? s_gravOnSeq : s_gravOffSeq).fetch_add(1, std::memory_order_relaxed);
        PLOG_INFO("[PULSAR] GRAV: engine {} on local player"sv,
                  a_on ? "ActionGravityOn" : "ActionGravityOff");
    }

    void Tick(bool isHost)
    {
        State& s = s_state;
        const char* tag = isHost ? "HOST" : "CLIENT";
        const auto& cfg = Pulsar::GetConfig();

        auto* player = RE::PlayerCharacter::GetSingleton();
        const GravSnap g = RawSnapGravity(player);
        if (!g.valid) return;

        RE::BSFixedString loadingMenuName("LoadingMenu");
        const bool inLoadMenu = RawCheckMenuOpen(RE::UI::GetSingleton(), loadingMenuName);
        const std::uint64_t now = ::GetTickCount64();

        // 1. Change log (rate limited) + transition arming.
        if (Differs(g, s.last)) {
            if (g.cellID != s.last.cellID) ArmTransitionCheck(s, now);
            if (now - s.lastChangeLog >= kChangeLogMinMs) {
                s.lastChangeLog = now;
                LogSnap(tag, "change", g);
            }
        }
        if (!inLoadMenu && s.lastInLoadMenu) ArmTransitionCheck(s, now);
        if (inLoadMenu) {
            // Never judge mid-load; push the check past the load screen.
            if (s.checkDueAt != 0) s.checkDueAt = now + kSettleMs;
        }
        s.lastInLoadMenu = inLoadMenu;

        // 2. Post-transition judgement.
        if (s.checkDueAt != 0 && now >= s.checkDueAt && !inLoadMenu) {
            s.checkDueAt = 0;
            const bool gravOnFired =
                s_gravOnSeq.load(std::memory_order_relaxed) != s.gravOnSeqAtStart;
            LogSnap(tag, gravOnFired ? "settled (engine gravity-on seen)" : "settled", g);

            const bool leftZeroG = s.zeroGAtStart && !g.zeroG && !g.inSpace;
            const bool connected = Pulsar::Network::GetSingleton().IsConnected();
            if (leftZeroG && !gravOnFired && !isHost && connected && cfg.coopGravityAutoFix) {
                const bool ok = FireGravityOn(player);
                PLOG_INFO(
                    "[PULSAR] GRAV: {} zero-G -> gravity transition without engine "
                    "ActionGravityOn - auto-fix fired={}"sv, tag, ok ? 1 : 0);
                NASLOG("GRAV %s auto-fix ActionGravityOn fired=%d", tag, ok ? 1 : 0);
                s.verifyDueAt = now + kVerifyMs;
            }
        }

        // 3. Manual repair hotkey (edge-triggered).
        {
            const bool down = (GetAsyncKeyState(cfg.hotkeyGravityFix) & 0x8000) != 0;
            if (down && !s.hotkeyDown && !inLoadMenu) {
                LogSnap(tag, "hotkey before", g);
                const bool ok = FireGravityOn(player);
                PLOG_INFO("[PULSAR] GRAV: {} hotkey ActionGravityOn fired={}"sv, tag, ok ? 1 : 0);
                s.verifyDueAt = now + kVerifyMs;
            }
            s.hotkeyDown = down;
        }

        if (s.verifyDueAt != 0 && now >= s.verifyDueAt) {
            s.verifyDueAt = 0;
            LogSnap(tag, "after repair", g);
        }

        s.last = g;
    }
}
