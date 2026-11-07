#pragma once

#include <cstddef>
#include <cstdint>

// Space clock sync (phase 6, flying together). Planets, stations and ships in
// space are not placed from a clock: every space object's orbit is advanced by
// each frame's dt. A client that loads the host's save starts from the save's
// state while the host's universe moves on, so after joining the client's whole
// universe runs ~21 s behind (the load time; GameDaysPassed is synced elsewhere
// and does not drive the orbits). The host sends its draw origin (its ship's
// position in the star system) twice a second (PKT_SPACE_CLOCK); the client works
// out how many seconds behind it is along the host's direction of motion and
// catches up once by adding that much game time to every space object's next
// update (hook on the per-object space update). CoopSpaceTimeSync = 0 turns it
// off. Investigation: docs/design/specs/2026-10-31-flying-together-ship-position.md.

namespace Pulsar::SpaceClockSync
{
    // kPostLoad, after MH_Initialize: installs the space-update hook.
    void Init();

    // Game thread, every tick: host sends, client measures and catches up.
    void Tick();

    // Network thread: a PKT_SPACE_CLOCK from the host (clients only).
    void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_len);

    // The local player ship's position in the star system (metres, doubles): the
    // origin space objects are drawn relative to. False outside space.
    bool ReadDrawOrigin(double a_out[3]);

    // Clients: true when this client's universe is known to run within a couple of
    // seconds of the host's (or the clock sync is off), so positions of other ships
    // can be compared with the host's. False while catching up, before the first
    // measurement and when the last one is older than 3 s.
    bool InStepWithHost();

    // How many catch-ups have run (changes when the whole universe jumps in time).
    int CatchUps();

    // Test bot: the last measured lag in seconds (positive: this client is behind
    // the host; NaN before the first measurement), and how many catch-ups ran.
    double LastLagForTest();
    int    CatchUpsForTest();
}
