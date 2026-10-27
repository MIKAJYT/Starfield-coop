#pragma once
#include <cstdint>

namespace Pulsar::Coop::GravityGuard
{
    // Game thread. Watches the LOCAL player's gravity state across transitions,
    // logs it, and (client, coopGravityAutoFix) re-applies ActionGravityOn when a
    // zero-G -> gravity transition left the engine's gravity-on step unfired.
    // Also polls the manual repair hotkey (hotkeyGravityFix).
    void Tick(bool isHost);

    // Called from the PerformAction hook (any thread) when the engine runs
    // ActionGravityOn / ActionGravityOff on the LOCAL player.
    void OnLocalGravityAction(bool a_on);

    // AACT FormIDs, verified by Starfield.esm scan (1.16.244, 2026-10-27).
    inline constexpr std::uint32_t kActionGravityOff = 0x00024654;
    inline constexpr std::uint32_t kActionGravityOn  = 0x00024655;
}
