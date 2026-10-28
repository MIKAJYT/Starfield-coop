// PULSAR.ini parsing (src/Config.cpp, ParseConfig). Every setting a tester is
// told to change lives here; a typo in a key name silently keeps the default.

#include "TestHarness.h"

#include "Config.h"
#include "VehicleSync/RidePolicy.h"

#include <sstream>

using Pulsar::Config;
using Pulsar::ParseConfig;

namespace {
    Config Parse(const std::string& a_ini)
    {
        std::istringstream in(a_ini);
        return ParseConfig(in);
    }
}

TEST(Config, EmptyFileGivesTheDefaults)
{
    const auto c = Parse("");
    const Config d;
    CHECK_EQ(c.isHost, d.isHost);
    CHECK_EQ(c.port, d.port);
    CHECK_EQ(c.hotkeyManualBridge, 0x77);   // F8
    CHECK_EQ(c.hotkeyBuddy, 0x78);          // F9
    CHECK(!c.autoRegroupInSpace);
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kOff));
    CHECK(c.syncCloneWeapons);
    CHECK(c.dbAuthoritative);
    CHECK(c.coopPartnerCountsAsFollower);
    CHECK(c.coopTraitNpcsForPartner);
    CHECK(!c.cloneUsePlayerStates);
    CHECK(c.coopGravityAutoFix);
    CHECK_EQ(c.hotkeyGravityFix, 0x73);     // F4
    CHECK(c.coopBridgeRestoreRetry);
    CHECK(!c.coopDoorFollow);               // spec: doors move only the user
    CHECK(c.coopInstancedEnemyLoot);        // spec: each player loots their own copy
    CHECK(c.coopQuestRewards);              // spec: quest rewards for everyone
    CHECK(c.coopTakeoffVote);               // spec: takeoff waits for a ready vote
    CHECK_EQ(c.hotkeyTakeoffReady, 0x72);   // F3
}

TEST(Config, TakeoffVoteSettingsAreRead)
{
    CHECK(!Parse("CoopTakeoffVote = 0\n").coopTakeoffVote);
    CHECK_EQ(Parse("HotkeyTakeoffReady = F2\n").hotkeyTakeoffReady, 0x71);
    CHECK(Parse("").coopVoteMessages);
    CHECK(!Parse("CoopVoteMessages = 0\n").coopVoteMessages);
}

TEST(Config, InstancedEnemyLootCanBeTurnedOff)
{
    CHECK(!Parse("CoopInstancedEnemyLoot = 0\n").coopInstancedEnemyLoot);
    CHECK(!Parse("coop_instanced_enemy_loot=off\n").coopInstancedEnemyLoot);
}

TEST(Config, QuestRewardsCanBeTurnedOff)
{
    CHECK(!Parse("CoopQuestRewards = 0\n").coopQuestRewards);
    CHECK(Parse("coop_quest_rewards=on\n").coopQuestRewards);
}

TEST(Config, DoorFollowCanBeTurnedBackOn)
{
    CHECK(Parse("CoopDoorFollow = 1\n").coopDoorFollow);
    CHECK(Parse("coop_door_follow=on\n").coopDoorFollow);
    CHECK(!Parse("CoopDoorFollow = sometimes\n").coopDoorFollow);
}

TEST(Config, GravityAndBridgeRetrySettingsAreRead)
{
    const auto c = Parse(
        "CoopGravityAutoFix = 0\n"
        "HotkeyGravityFix = F6\n"
        "coop_bridge_restore_retry = off\n");
    CHECK(!c.coopGravityAutoFix);
    CHECK_EQ(c.hotkeyGravityFix, 0x75);
    CHECK(!c.coopBridgeRestoreRetry);
}

TEST(Config, BadGravityHotkeyKeepsTheDefault)
{
    const auto c = Parse("HotkeyGravityFix = NotAKey\n");
    CHECK_EQ(c.hotkeyGravityFix, 0x73);
}

TEST(Config, NewSettingsAreRead)
{
    const auto c = Parse(
        "[PULSAR]\n"
        "AutoRegroupInSpace = 1\n"
        "SharedVehicleRide = carry\n"
        "SyncCloneWeapons = 0\n"
        "HotkeyBuddy = F6\n"
        "BuddyBaseFormID = 0x0001234A\n"
        "CoopPartnerCountsAsFollower = 0\n");
    CHECK(c.autoRegroupInSpace);
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kCarry));
    CHECK(!c.syncCloneWeapons);
    CHECK_EQ(c.hotkeyBuddy, 0x75);
    CHECK_EQ(c.buddyBaseFormID, 0x0001234Au);
    CHECK(!c.coopPartnerCountsAsFollower);
}

TEST(Config, KeysAreCaseInsensitiveAndAcceptSnakeCase)
{
    const auto c = Parse("auto_regroup_in_space=true\nSHAREDVEHICLERIDE=enter\nsync_clone_weapons=off\n");
    CHECK(c.autoRegroupInSpace);
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kEnter));
    CHECK(!c.syncCloneWeapons);
}

TEST(Config, CommentsAndJunkAreIgnored)
{
    const auto c = Parse(
        "; a comment\n"
        "# another\n"
        "garbage line without equals\n"
        "UnknownKey = 5\n"
        "AutoRegroupInSpace = 1 ; inline comment\n"
        "  SharedVehicleRide   =   carry   \r\n");
    CHECK(c.autoRegroupInSpace);
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kCarry));
}

TEST(Config, BadValuesKeepSafeDefaults)
{
    const auto c = Parse("SharedVehicleRide = sometimes\nHotkeyBuddy = NotAKey\n");
    CHECK_EQ(c.sharedVehicleRide, int(Pulsar::VehicleSync::RideMode::kOff));
    CHECK_EQ(c.hotkeyBuddy, 0x78);
}
