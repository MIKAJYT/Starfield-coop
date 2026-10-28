#pragma once

#include <cstddef>
#include <cstdint>

namespace Pulsar::Coop::TakeoffVote {

    // Takeoff ready vote (CoopTakeoffVote; policy in Coop/TakeoffVotePolicy.h).
    //
    // Pilot: SpaceSync's TakeoffMenu_Launch sink calls HoldLaunch for a local
    // launch. True = hold it (the sink stops the event before the takeoff menu
    // sees it) while the partner is asked. When the partner is ready the pilot
    // sends "go", waits for the partner to board, then re-fires the launch
    // through SpaceSync::ReleaseHeldLaunch, which the sink lets through.
    //
    // Partner: a request shows a message; HotkeyTakeoffReady answers ready.
    // On "go" the partner is moved aboard (SpaceSync::BoardPartnerShip) unless
    // it already is.
    bool HoldLaunch();
    void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_length);
    void Tick();   // game thread: hotkey, timeout, launch, boarding

    // The pilot left the seat (takeoff menu closed).
    void OnTakeoffMenuClosed();

    // Test bot.
    std::uint32_t PendingRequestForTest();   // partner: open request id, 0 if none
    void          AnswerReadyForTest();
    const char*   StateForTest();            // "idle" / "waiting" / "boarding"
    void          ShowMessageForTest(const char* a_msg);   // queue an on-screen message

}  // namespace Pulsar::Coop::TakeoffVote
