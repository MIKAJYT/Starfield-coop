#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Scene sync (phase 5, cutscenes). The engine's TESSceneEvent source is found by
// its vtable and every scene start and end is logged ([SCENE]). The host sends
// each start and end to the clients (PKT_SCENE); a client starts the same scene,
// its quest first when that isn't running there, or stops it. CoopSceneSync = 0
// turns the sending and applying off.

namespace Pulsar::SceneSync
{
    // kPostLoad, with the other event-sink modules.
    void Init();

    // Network thread: a PKT_SCENE from the host (clients only).
    void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_len);

    // Game thread, every tick: apply the host's scene starts and ends.
    void DrainPending();

    // Test bot (Debug/TestBot): the last scene events this machine saw, oldest
    // first, as "SCENEID:TYPE:WALLMS,..." (TYPE 0 started, 1 ended).
    std::string RecentEventsForTest();

    // Test bot: start a scene here as a client does for the host's (quest first
    // when needed). False when a_id is no scene. Game thread.
    bool StartSceneForTest(std::uint32_t a_id);

    // Test bot: wall time (ms) of the last start of a_id seen here, 0 if none.
    std::uint64_t LastStartWallMsForTest(std::uint32_t a_id);

    // Test bot: scenes whose owning quest is running here and whose editor ID
    // contains a_hint (case-insensitive), lowest form ID first, at most a_max.
    // Background quests can only be started by the story manager, so a test
    // scene has to come from a quest that already runs. a_info says how the
    // owning-quest field was found. Game thread.
    std::vector<std::uint32_t> RunningScenesForTest(const char* a_hint, std::size_t a_max, std::string& a_info);
}
