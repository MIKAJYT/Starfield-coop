Scriptname PulsarTestShip extends ScriptObject Hidden

; Test-only helper for the co-op bot (Debug/TestBot, scenario "dock"), called
; from C++ via DispatchStaticCall. C++ can only hand the ship over as an
; ObjectReference, and InstantDock / IsDocked live on SpaceshipReference, so the
; cast happens here. Not part of the release package: the launcher copies the
; compiled .pex into the two test game copies only.

; Docks akShip with akTarget (another ship or a docking port) at once.
bool Function Dock(ObjectReference akShip, ObjectReference akTarget) Global
    SpaceshipReference ship = akShip as SpaceshipReference
    if !ship || !akTarget
        return false
    endif
    return ship.InstantDock(akTarget)
EndFunction

; Whether akShip is docked with anything.
bool Function IsShipDocked(ObjectReference akShip) Global
    SpaceshipReference ship = akShip as SpaceshipReference
    if !ship
        return false
    endif
    return ship.IsDocked()
EndFunction
