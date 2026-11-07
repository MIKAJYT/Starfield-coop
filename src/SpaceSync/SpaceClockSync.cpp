#include "PCH.h"
#include "SpaceSync/SpaceClockSync.h"

#include "Config.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"

#include <RE/B/BSFixedString.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESGlobal.h>

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_set>

using namespace std::string_view_literals;

namespace Pulsar::SpaceClockSync {
namespace {

    // The draw origin: [[Starfield+0x5FB08E8] + 0x90] + 0x110, 3 doubles (the owner
    // is loaded from that global by the caller at +0x18849BE; FlightSync uses the
    // same origin to find its own ship body). 1.16.244.
    constexpr std::uintptr_t kRVA_SpaceDrawOriginOwner = 0x5FB08E8;

    // Per-object space update (+0x234A5D0, called from the space job at
    // +0x234BCAB): rcx = space object, xmm1 = dt in game seconds (frame time x
    // TimeScale), r8 = parent position, xmm3, two stack arguments. It advances the
    // object's orbit by dt, so a larger dt moves it on in time.
    constexpr std::uintptr_t kRVA_SpaceObjectUpdate = 0x234A5D0;
    using SpaceObjectUpdate_t = std::uint64_t(__fastcall*)(void*, float, void*, float, std::uint64_t, std::uint32_t);
    SpaceObjectUpdate_t g_origUpdate = nullptr;

    // Catch-up: for kWarpWindowMs every space object gets g_warpGameSec added to
    // its dt exactly once (each updates every frame, so all of them get it).
    constexpr std::uint64_t kWarpWindowMs = 150;
    std::atomic<std::uint64_t> g_warpUntilMs{ 0 };
    float                      g_warpGameSec = 0;
    std::mutex                 g_warpMutex;
    std::unordered_set<void*>  g_warped;

    std::uint64_t __fastcall HookedSpaceObjectUpdate(void* a_obj, float a_dt, void* a_parent, float a_4,
                                                     std::uint64_t a_5, std::uint32_t a_6)
    {
        const std::uint64_t until = g_warpUntilMs.load(std::memory_order_acquire);
        if (until != 0 && ::GetTickCount64() < until) {
            std::lock_guard lock(g_warpMutex);
            if (g_warped.insert(a_obj).second) a_dt += g_warpGameSec;
        }
        return g_origUpdate(a_obj, a_dt, a_parent, a_4, a_5, a_6);
    }

    // Host samples on the client (network thread writes, game thread reads).
    struct Sample
    {
        double        origin[3];
        std::uint64_t recvMs;
    };
    std::mutex g_sampleMutex;
    Sample     g_samples[2]{};
    int        g_sampleCount = 0;

    constexpr std::uint64_t kSendIntervalMs  = 500;
    constexpr std::uint64_t kMeasureEveryMs  = 1000;
    constexpr std::uint64_t kWarpGapMs       = 3000;   // measure again before the next catch-up
    constexpr double        kLagToleranceS   = 0.5;
    constexpr double        kLagMaxS         = 600.0;  // more is no load lag: leave it alone
    constexpr double        kMaxSideOffsetM  = 20000;  // off the host's track: not the same place
    constexpr int           kMaxCatchUps     = 5;      // per lag episode
    constexpr double        kAimFraction     = 0.96;   // of the measured lag

    std::atomic<double> g_lastLag{ std::numeric_limits<double>::quiet_NaN() };
    std::atomic<int>    g_catchUps{ 0 };

    bool SafeReadOrigin(double a_out[3]) noexcept
    {
        __try {
            const std::uintptr_t owner =
                *reinterpret_cast<std::uintptr_t*>(REL::Offset{ kRVA_SpaceDrawOriginOwner }.address());
            if (!owner) return false;
            const auto* o = reinterpret_cast<const double*>(owner + 0x90 + 0x110);
            if (o[0] == 0.0 && o[1] == 0.0 && o[2] == 0.0) return false;  // not in space
            for (int k = 0; k < 3; ++k) {
                if (!std::isfinite(o[k])) return false;
                a_out[k] = o[k];
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    float TimeScale()
    {
        static RE::TESGlobal* s_scale = RE::TESForm::LookupByEditorID<RE::TESGlobal>(RE::BSFixedString("TimeScale"));
        const float v = s_scale ? s_scale->value : 0.0f;
        return v > 0.0f ? v : 1.0f;
    }

    void HostTick(std::uint64_t a_now)
    {
        static std::uint64_t s_lastSendMs = 0;
        if (a_now - s_lastSendMs < kSendIntervalMs) return;
        double origin[3];
        if (!SafeReadOrigin(origin)) return;
        s_lastSendMs = a_now;
        std::uint8_t buf[1 + sizeof(origin)];
        buf[0] = Pulsar::Network::PKT_SPACE_CLOCK;
        std::memcpy(buf + 1, origin, sizeof(origin));
        Pulsar::Network::GetSingleton().Send(buf, sizeof(buf), /*reliable=*/false);
    }

    void ClientTick(std::uint64_t a_now)
    {
        // Close a catch-up window and log it.
        const std::uint64_t until = g_warpUntilMs.load(std::memory_order_acquire);
        if (until != 0 && a_now > until + 50) {
            std::size_t n = 0;
            {
                std::lock_guard lock(g_warpMutex);
                n = g_warped.size();
                g_warped.clear();
            }
            g_warpUntilMs.store(0, std::memory_order_release);
            PLOG_INFO("[PULSAR] SpaceClock: catch-up of {:.1f} game s applied to {} space objects"sv, g_warpGameSec, n);
        }

        static std::uint64_t s_lastMeasureMs = 0;
        static std::uint64_t s_lastWarpMs = 0;
        static std::uint64_t s_lastLogMs = 0;
        static int           s_episodeCatchUps = 0;
        if (a_now - s_lastMeasureMs < kMeasureEveryMs) return;
        s_lastMeasureMs = a_now;

        Sample h1{}, h2{};
        {
            std::lock_guard lock(g_sampleMutex);
            if (g_sampleCount < 2) return;
            h1 = g_samples[0];
            h2 = g_samples[1];
        }
        const double gapS = static_cast<double>(h2.recvMs - h1.recvMs) / 1000.0;
        if (gapS < 0.2 || gapS > 3.0 || a_now - h2.recvMs > 1500) return;  // stale or too close
        double own[3];
        if (!SafeReadOrigin(own)) return;

        // Host velocity from its last two samples; host position extrapolated to now.
        double v[3], d[3];
        double v2 = 0;
        const double sinceS = static_cast<double>(a_now - h2.recvMs) / 1000.0;
        for (int k = 0; k < 3; ++k) {
            v[k] = (h2.origin[k] - h1.origin[k]) / gapS;
            v2 += v[k] * v[k];
        }
        if (v2 < 1.0) return;  // the host's frame does not move: nothing to measure by
        double along = 0;
        for (int k = 0; k < 3; ++k) {
            d[k] = h2.origin[k] + v[k] * sinceS - own[k];
            along += d[k] * v[k];
        }
        const double lag = along / v2;  // seconds; positive: this client is behind
        double side2 = 0;
        for (int k = 0; k < 3; ++k) {
            const double s = d[k] - lag * v[k];
            side2 += s * s;
        }
        const double side = std::sqrt(side2);
        g_lastLag.store(lag, std::memory_order_relaxed);

        if (a_now - s_lastLogMs >= 10000) {
            s_lastLogMs = a_now;
            PLOG_INFO("[PULSAR] SpaceClock: lag {:.2f} s (positive = behind the host), off track {:.0f} m, host speed {:.0f} m/s"sv,
                      lag, side, std::sqrt(v2));
        }
        if (side > kMaxSideOffsetM) return;  // not on the host's track: some other place
        if (std::fabs(lag) <= kLagToleranceS) {
            s_episodeCatchUps = 0;  // in step: a later lag (a new load) may catch up again
            return;
        }
        if (lag < 0) return;  // ahead: time cannot run backwards here
        if (lag > kLagMaxS || s_episodeCatchUps >= kMaxCatchUps) return;
        if (a_now - s_lastWarpMs < kWarpGapMs || !g_origUpdate) return;

        s_lastWarpMs = a_now;
        ++s_episodeCatchUps;
        g_catchUps.fetch_add(1, std::memory_order_relaxed);
        // Aim 4 % short: 111.6 game s for a 22.32 s lag overshot to -0.60 s (run
        // 20261107-162813), and an overshoot cannot be undone; a remainder behind is
        // caught up by the next attempt.
        g_warpGameSec = static_cast<float>(lag * TimeScale() * kAimFraction);
        {
            std::lock_guard lock(g_warpMutex);
            g_warped.clear();
        }
        g_warpUntilMs.store(a_now + kWarpWindowMs, std::memory_order_release);
        PLOG_INFO("[PULSAR] SpaceClock: {:.2f} s behind the host, catching up {:.1f} game s (attempt {})"sv, lag,
                  g_warpGameSec, s_episodeCatchUps);
    }

}  // namespace

void Init()
{
    auto* target = reinterpret_cast<LPVOID>(REL::Offset{ kRVA_SpaceObjectUpdate }.address());
    const MH_STATUS c = MH_CreateHook(target, reinterpret_cast<LPVOID>(&HookedSpaceObjectUpdate),
                                      reinterpret_cast<LPVOID*>(&g_origUpdate));
    const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
    if (e != MH_OK) g_origUpdate = nullptr;
    PLOG_INFO("[PULSAR] SpaceClock: space update hook create={} enable={}"sv, static_cast<int>(c), static_cast<int>(e));
}

void Tick()
{
    if (!Pulsar::GetConfig().coopSpaceTimeSync) return;
    auto& net = Pulsar::Network::GetSingleton();
    if (!net.IsConnected()) return;
    const std::uint64_t now = ::GetTickCount64();
    if (net.IsHost()) {
        HostTick(now);
    } else {
        ClientTick(now);
    }
}

void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_len)
{
    if (!a_data || a_len < 1 + 3 * sizeof(double) || a_data[0] != Pulsar::Network::PKT_SPACE_CLOCK) return;
    Sample s{};
    std::memcpy(s.origin, a_data + 1, sizeof(s.origin));
    s.recvMs = ::GetTickCount64();
    for (double c : s.origin) {
        if (!std::isfinite(c)) return;
    }
    std::lock_guard lock(g_sampleMutex);
    g_samples[0] = g_samples[1];
    g_samples[1] = s;
    if (g_sampleCount < 2) ++g_sampleCount;
}

bool ReadDrawOrigin(double a_out[3])
{
    return SafeReadOrigin(a_out);
}

double LastLagForTest()
{
    return g_lastLag.load(std::memory_order_relaxed);
}

int CatchUpsForTest()
{
    return g_catchUps.load(std::memory_order_relaxed);
}

}  // namespace Pulsar::SpaceClockSync
