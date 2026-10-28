#pragma once

#include <cstdint>
#include <cstddef>

namespace Pulsar::SpaceSync {

    // K.7 V1 - Sink onto Spaceship engine events (TakeOff, Landing, GravJump,
    // Dock, TakeoffMenu_Launch, RampDown, FarTravel) via SFSE-documented event
    // sources. On host fire, broadcast PKT_SPACE_TRANSITION.
    //
    // K.7 V2 - Receiver auto-follows: when peer event arrives + F7 follow
    // enabled, MoveTo peer's clone so engine switches client to peer's
    // worldspace+cell (cockpit + space WS, or cockpit + planet WS for landing).
    void Init();

    // Receiver entry - called by NetworkHost dispatch for PKT_SPACE_TRANSITION.
    void OnPacketReceived(const std::uint8_t* data, std::size_t length);

    // L.3 V1 - receiver entry for PKT_SHIP_POS (ship pos/rot updates).
    void OnShipPosReceived(const std::uint8_t* data, std::size_t length);

    // Game-thread tick - applies pending auto-follow if armed.
    void TickFollowApply();

    // Test bot: fire the engine's TakeoffMenu_Launch event through its helper
    // (RVA 0x1677090), as a local launch (our sink broadcasts it). Returns false
    // if the helper threw.
    bool FireTakeoffLaunchForTest();

    // Test bot: open the engine's takeoff menu for the seated pilot
    // (LaunchImmediate, RVA 0x16757D0, as the receiver's step 1). The launch
    // itself is FireTakeoffLaunchForTest. Run 20261028-203830: the two together
    // take the ship off.
    void OpenTakeoffMenuForTest();

    // Takeoff ready vote: re-fire the launch the vote held; the sink lets it
    // through. False if the engine helper threw.
    bool ReleaseHeldLaunch();

    // Takeoff ready vote, partner: move the local player to the pilot's clone
    // (Papyrus MoveTo) unless both are in the same cell. Returns what it did.
    const char* BoardPartnerShip();

}  // namespace Pulsar::SpaceSync
