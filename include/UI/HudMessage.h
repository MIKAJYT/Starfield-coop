#pragma once

namespace Pulsar::UI::HudMessage {

    // On-screen notification (top right, like the game's own), without the
    // Papyrus VM: C++ DispatchStaticCall of Debug.Notification faulted inside
    // the engine and ended the game (run 20261028-214018). Show queues the text
    // from any thread; Tick (game thread) hands the queue to a worker thread
    // that calls the Debug.Notification native directly. Details and evidence in
    // src/UI/HudMessage.cpp. Starfield 1.16.244 only; on another build the
    // native's prologue does not match and messages are dropped (logged).
    void Show(const char* a_text);
    void Tick();

}  // namespace Pulsar::UI::HudMessage
