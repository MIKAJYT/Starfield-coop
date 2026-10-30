#pragma once

// Automated co-op QA bot (coop-anything playbook/automated-qa.md). Test copies
// only: it does nothing unless PulsarTestBot.ini sits next to PULSAR.dll, and it
// is compiled out of the hardened Release build entirely.
//
// Every second it appends this copy's view of the co-op session to a state log
// (role, in world, connected, own position and cell, the partner clone and its
// position) with wall-clock timestamps, so a launcher running two copies on one
// PC can compare what each side sees. The host bot also performs the test's
// action: once the partner's clone has been visible for a while, it moves the
// host player a few metres, so the launcher can check the client's clone follows.
namespace Pulsar::TestBot {

    // Game-thread tick (cheap: one file read at start, one state write per second).
    void Tick();

    // Aim scenario (host): the look pitch and aiming flag the bot wants sent in place
    // of the player's own. False when the bot is not steering the aim.
    bool AimOverrideForTest(float& a_pitch, bool& a_sighted);

}  // namespace Pulsar::TestBot
