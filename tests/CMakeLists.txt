# pulsar_tests: game-free unit and loopback tests. Builds next to PULSAR.dll and
# runs without Starfield. See docs/TESTING.md.
# pulsar_fakepeer: headless PULSAR client for one-PC testing against the game
# (tests/fakepeer, docs/TESTING.md "Fake peer"). Same game-free build setup.

# Shared settings for the game-free executables.
function(pulsar_game_free_target a_target)
  # tests/stub comes FIRST so `#include "PCH.h"` in the production sources picks
  # up the game-free stand-in instead of include/PCH.h.
  target_include_directories(
    ${a_target}
    BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stub
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
  )
  target_compile_features(${a_target} PRIVATE cxx_std_23)
  target_compile_options(${a_target} PRIVATE /W4 /utf-8 /EHsc /permissive- /Zc:__cplusplus)
  target_compile_definitions(
    ${a_target}
    PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    "PULSAR_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
    PULSAR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PULSAR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PULSAR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  )
  target_link_libraries(${a_target} PRIVATE unofficial::enet::enet bcrypt ws2_32 winmm)
endfunction()

add_executable(
  pulsar_tests
  main.cpp
  test_profile_codec.cpp
  test_crypto.cpp
  test_relay_and_packets.cpp
  test_loopback.cpp
  test_fakepeer.cpp
  test_version_compat.cpp
  test_divergence_watch.cpp
  test_profile_merge.cpp
  test_ride_policy.cpp
  test_match_policy.cpp
  test_reward_ledger.cpp
  test_stage_journal.cpp
  test_config.cpp
  test_follower_bonus.cpp
  test_trait_aliases.cpp
  test_key_exchange.cpp
  test_direction_map.cpp
  test_harness_policy.cpp
  test_harness_selftest.cpp
  fakepeer/FakePeer.cpp
  # Production sources under test (must stay free of game headers):
  ${PROJECT_SOURCE_DIR}/src/Profile/ProfileCodec.cpp
  ${PROJECT_SOURCE_DIR}/src/Profile/ProfileMerge.cpp
  ${PROJECT_SOURCE_DIR}/src/Network/Crypto.cpp
  ${PROJECT_SOURCE_DIR}/src/Config.cpp
)
pulsar_game_free_target(pulsar_tests)

add_executable(
  pulsar_fakepeer
  fakepeer/main.cpp
  fakepeer/FakePeer.cpp
  ${PROJECT_SOURCE_DIR}/src/Network/Crypto.cpp
  ${PROJECT_SOURCE_DIR}/src/Config.cpp
)
pulsar_game_free_target(pulsar_fakepeer)

add_test(NAME pulsar_tests COMMAND pulsar_tests)

# PulsarTestHarness.dll: TEST-ONLY SFSE plugin for hard-linked test copies of the
# game (folder redirect + single-instance workaround, tests/harness/Harness.cpp).
# Never packaged: tools/package_release.ps1 ships Release\PULSAR.dll by name.
add_library(pulsar_test_harness SHARED harness/Harness.cpp)
set_target_properties(pulsar_test_harness PROPERTIES OUTPUT_NAME PulsarTestHarness)
target_compile_features(pulsar_test_harness PRIVATE cxx_std_23)
target_compile_options(pulsar_test_harness PRIVATE /W4 /utf-8 /EHsc /permissive- /Zc:__cplusplus)
target_compile_definitions(pulsar_test_harness PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulsar_test_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pulsar_test_harness PRIVATE minhook::minhook shell32 ole32 user32)

# Test-only stand-in for steam_api64.dll: a fake ISteamRemoteStorage v016 that
# counts the cloud writes reaching it. Output name steam_api64.dll so the fake
# game imports it the way Starfield.exe imports the real one.
add_library(pulsar_fakesteamapi SHARED harness/FakeSteamApi.cpp)
set_target_properties(pulsar_fakesteamapi PROPERTIES OUTPUT_NAME steam_api64)
target_compile_features(pulsar_fakesteamapi PRIVATE cxx_std_23)
target_compile_options(pulsar_fakesteamapi PRIVATE /W4 /utf-8 /EHsc /permissive-)

# Stand-in game for the harness self-test (Starfield.exe's single-instance checks
# and its Steam Cloud route).
add_executable(pulsar_fakegame harness/FakeGame.cpp)
target_compile_features(pulsar_fakegame PRIVATE cxx_std_23)
target_compile_options(pulsar_fakegame PRIVATE /W4 /utf-8 /EHsc /permissive-)
target_compile_definitions(pulsar_fakegame PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulsar_fakegame PRIVATE shell32 ole32 user32 pulsar_fakesteamapi)

add_dependencies(pulsar_tests pulsar_test_harness pulsar_fakegame)
target_compile_definitions(
  pulsar_tests
  PRIVATE
  "PULSAR_HARNESS_DLL=\"$<TARGET_FILE:pulsar_test_harness>\""
  "PULSAR_FAKEGAME_EXE=\"$<TARGET_FILE:pulsar_fakegame>\""
)
