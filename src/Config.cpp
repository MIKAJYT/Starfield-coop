#include "PCH.h"
#include "Config.h"
#include "VehicleSync/RidePolicy.h"

#include <fstream>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

using namespace std::string_view_literals;

namespace Pulsar
{
    static std::string Trim(const std::string& s)
    {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    // "1", "true", "yes" or "on" (any case) is true; anything else false.
    static bool ParseBool(const std::string& a_value)
    {
        std::string v = a_value;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }
    int ParseVKey(const std::string& a_input)
    {
        std::string in = Trim(a_input);
        if (in.empty()) return -1;

        // Numeric forms: "0x77" / "119"
        if (in.size() > 2 && (in[0] == '0') && (in[1] == 'x' || in[1] == 'X')) {
            try { return std::stoi(in.substr(2), nullptr, 16); }
            catch (...) { return -1; }
        }
        if (std::all_of(in.begin(), in.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            try { return std::stoi(in, nullptr, 10); }
            catch (...) { return -1; }
        }

        // Symbolic forms - case-insensitive
        std::string up = in;
        std::transform(up.begin(), up.end(), up.begin(), ::toupper);

        // F1..F24
        if (up.size() >= 2 && up[0] == 'F') {
            try {
                int n = std::stoi(up.substr(1));
                if (n >= 1 && n <= 24) return 0x70 + (n - 1);  // VK_F1=0x70
            } catch (...) {}
        }
        // Numpad0..Numpad9
        if (up.rfind("NUMPAD", 0) == 0 && up.size() == 7 && std::isdigit(static_cast<unsigned char>(up[6]))) {
            return 0x60 + (up[6] - '0');  // VK_NUMPAD0=0x60
        }
        // Single-char A-Z / 0-9
        if (up.size() == 1) {
            char c = up[0];
            if (c >= 'A' && c <= 'Z') return c;          // VK_A=0x41 = 'A'
            if (c >= '0' && c <= '9') return c;          // VK_0=0x30 = '0'
        }
        return -1;
    }

    Config ParseConfig(std::istream& a_in)
    {
        Config cfg;
        std::string line;
        while (std::getline(a_in, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') {
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));

            // Strip inline comments
            auto comment = value.find(';');
            if (comment != std::string::npos) {
                value = Trim(value.substr(0, comment));
            }

            std::string keyLower = key;
            std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);

            if (keyLower == "mode") {
                std::string valLower = value;
                std::transform(valLower.begin(), valLower.end(), valLower.begin(), ::tolower);
                cfg.isHost = (valLower != "client");
                PLOG_INFO("[PULSAR] Config: Mode = {}"sv, cfg.isHost ? "host" : "client");
            }
            else if (keyLower == "hostip" || keyLower == "host") {
                cfg.hostIP = value;
                PLOG_INFO("[PULSAR] Config: HostIP = {}"sv, cfg.hostIP);
            }
            else if (keyLower == "port") {
                cfg.port = static_cast<uint16_t>(std::stoi(value));
                PLOG_INFO("[PULSAR] Config: Port = {}"sv, cfg.port);
            }
            else if (keyLower == "playername") {
                cfg.playerName = value;
                PLOG_INFO("[PULSAR] Config: PlayerName = {}"sv, cfg.playerName);
            }
            else if (keyLower == "playerlevel") {
                cfg.playerLevel = static_cast<uint32_t>(std::stoi(value));
                PLOG_INFO("[PULSAR] Config: PlayerLevel = {}"sv, cfg.playerLevel);
            }
            else if (keyLower == "racename") {
                cfg.raceName = value;
                PLOG_INFO("[PULSAR] Config: RaceName = {}"sv, cfg.raceName);
            }
            else if (keyLower == "gender") {
                cfg.gender = static_cast<uint16_t>(std::stoi(value));
                PLOG_INFO("[PULSAR] Config: Gender = {}"sv, cfg.gender);
            }
            else if (keyLower == "hotkeyautofollow" || keyLower == "hotkey_autofollow") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyAutoFollow = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyAutoFollow = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyAutoFollow value '{}' unparseable - keeping default 0x{:02X}"sv,
                              value, cfg.hotkeyAutoFollow);
                }
            }
            else if (keyLower == "hotkeymanualbridge" || keyLower == "hotkey_manualbridge") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyManualBridge = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyManualBridge = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyManualBridge value '{}' unparseable - keeping default 0x{:02X}"sv,
                              value, cfg.hotkeyManualBridge);
                }
            }
            else if (keyLower == "hotkeysaveroundtrip" || keyLower == "hotkey_saveroundtrip") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeySaveRoundTrip = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeySaveRoundTrip = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeySaveRoundTrip value '{}' unparseable - keeping default 0x{:02X}"sv,
                              value, cfg.hotkeySaveRoundTrip);
                }
            }
            else if (keyLower == "hotkeysavediff" || keyLower == "hotkey_savediff") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeySaveDiff = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeySaveDiff = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeySaveDiff value '{}' unparseable - keeping default 0x{:02X}"sv,
                              value, cfg.hotkeySaveDiff);
                }
            }
            else if (keyLower == "hotkeysavecharacter" || keyLower == "hotkey_savecharacter") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeySaveCharacter = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeySaveCharacter = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeySaveCharacter '{}' unparseable - keeping 0x{:02X}"sv,
                              value, cfg.hotkeySaveCharacter);
                }
            }
            else if (keyLower == "hotkeyrestorecharacter" || keyLower == "hotkey_restorecharacter") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyRestoreCharacter = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyRestoreCharacter = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyRestoreCharacter '{}' unparseable - keeping 0x{:02X}"sv,
                              value, cfg.hotkeyRestoreCharacter);
                }
            }
            else if (keyLower == "hotkeybuddy" || keyLower == "hotkey_buddy") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyBuddy = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyBuddy = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyBuddy '{}' unparseable - keeping 0x{:02X}"sv,
                              value, cfg.hotkeyBuddy);
                }
            }
            else if (keyLower == "buddybaseformid" || keyLower == "buddy_base_formid") {
                cfg.buddyBaseFormID = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
                PLOG_INFO("[PULSAR] Config: BuddyBaseFormID = 0x{:08X}"sv, cfg.buddyBaseFormID);
            }
            else if (keyLower == "devhotkeys" || keyLower == "dev_hotkeys") {
                cfg.devHotkeys = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: DevHotkeys = {}"sv, cfg.devHotkeys);
            }
            else if (keyLower == "hotkeydevflashlightdump") {
                int vk = ParseVKey(value); if (vk > 0) cfg.hotkeyDevFlashlightDump = vk;
            }
            else if (keyLower == "hotkeydevdiag") {
                int vk = ParseVKey(value); if (vk > 0) cfg.hotkeyDevDiag = vk;
            }
            else if (keyLower == "hotkeydevmanualrevive") {
                int vk = ParseVKey(value); if (vk > 0) cfg.hotkeyDevManualRevive = vk;
            }
            else if (keyLower == "npcdirectcontrol" || keyLower == "npc_directcontrol") {
                cfg.npcDirectControl = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: NpcDirectControl = {}"sv, cfg.npcDirectControl);
            }
            else if (keyLower == "npcnoimc" || keyLower == "npc_noimc") {
                cfg.npcNoImc = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: NpcNoImc = {}"sv, cfg.npcNoImc);
            }
            else if (keyLower == "npccombatailive" || keyLower == "npc_combatailive") {
                cfg.npcCombatAiLive = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: NpcCombatAiLive = {}"sv, cfg.npcCombatAiLive);
            }
            else if (keyLower == "dbauthoritative" || keyLower == "db_authoritative") {
                cfg.dbAuthoritative = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: DbAuthoritative = {}"sv, cfg.dbAuthoritative);
            }
            else if (keyLower == "synccloneweapons" || keyLower == "sync_clone_weapons") {
                cfg.syncCloneWeapons = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: SyncCloneWeapons = {}"sv, cfg.syncCloneWeapons);
            }
            else if (keyLower == "coopgravityautofix" || keyLower == "coop_gravity_auto_fix") {
                cfg.coopGravityAutoFix = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopGravityAutoFix = {}"sv, cfg.coopGravityAutoFix);
            }
            else if (keyLower == "hotkeygravityfix" || keyLower == "hotkey_gravityfix") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyGravityFix = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyGravityFix = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyGravityFix '{}' unparseable - keeping 0x{:02X}"sv,
                              value, cfg.hotkeyGravityFix);
                }
            }
            else if (keyLower == "coopinstancedenemyloot" || keyLower == "coop_instanced_enemy_loot") {
                cfg.coopInstancedEnemyLoot = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopInstancedEnemyLoot = {}"sv, cfg.coopInstancedEnemyLoot);
            }
            else if (keyLower == "cooptakeoffvote" || keyLower == "coop_takeoff_vote") {
                cfg.coopTakeoffVote = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopTakeoffVote = {}"sv, cfg.coopTakeoffVote);
            }
            else if (keyLower == "cooptakeoffskipseated" || keyLower == "coop_takeoff_skip_seated") {
                cfg.coopTakeoffSkipSeated = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopTakeoffSkipSeated = {}"sv, cfg.coopTakeoffSkipSeated);
            }
            else if (keyLower == "coopvotemessages" || keyLower == "coop_vote_messages") {
                cfg.coopVoteMessages = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopVoteMessages = {}"sv, cfg.coopVoteMessages);
            }
            else if (keyLower == "maxplayers" || keyLower == "max_players") {
                const long n = std::strtol(value.c_str(), nullptr, 10);
                if (n >= 2 && n <= 4) cfg.maxPlayers = static_cast<std::uint32_t>(n);
                PLOG_INFO("[PULSAR] Config: MaxPlayers = {}"sv, cfg.maxPlayers);
            }
            else if (keyLower == "extraplayerfigures" || keyLower == "extra_player_figures") {
                cfg.extraPlayerFigures = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: ExtraPlayerFigures = {}"sv, cfg.extraPlayerFigures);
            }
            else if (keyLower == "directsession" || keyLower == "direct_session") {
                cfg.directSession = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: DirectSession = {}"sv, cfg.directSession);
            }
            else if (keyLower == "joincode" || keyLower == "join_code") {
                cfg.joinCode = value;
                PLOG_INFO("[PULSAR] Config: JoinCode set ({} characters)"sv, cfg.joinCode.size());
            }
            else if (keyLower == "replycode" || keyLower == "reply_code") {
                cfg.replyCode = value;
                PLOG_INFO("[PULSAR] Config: ReplyCode set ({} characters)"sv, cfg.replyCode.size());
            }
            else if (keyLower == "stunservers" || keyLower == "stun_servers") {
                cfg.stunServers = value;
                PLOG_INFO("[PULSAR] Config: StunServers = {}"sv, cfg.stunServers);
            }
            else if (keyLower == "useupnp" || keyLower == "use_upnp") {
                cfg.useUpnp = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: UseUpnp = {}"sv, cfg.useUpnp);
            }
            else if (keyLower == "copycodetoclipboard" || keyLower == "copy_code_to_clipboard") {
                cfg.copyCodeToClipboard = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CopyCodeToClipboard = {}"sv, cfg.copyCodeToClipboard);
            }
            else if (keyLower == "hotkeypastereplycode" || keyLower == "hotkey_paste_reply_code") {
                std::string v = value;
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                const int vk = (v == "0" || v == "off" || v == "none") ? 0 : ParseVKey(value);
                if (vk >= 0) cfg.hotkeyPasteReplyCode = vk;
                PLOG_INFO("[PULSAR] Config: HotkeyPasteReplyCode = {} (VK 0x{:02X})"sv, value, cfg.hotkeyPasteReplyCode);
            }
            else if (keyLower == "hotkeytakeoffready" || keyLower == "hotkey_takeoff_ready") {
                int vk = ParseVKey(value);
                if (vk > 0) {
                    cfg.hotkeyTakeoffReady = vk;
                    PLOG_INFO("[PULSAR] Config: HotkeyTakeoffReady = {} (VK 0x{:02X})"sv, value, vk);
                } else {
                    PLOG_WARN("[PULSAR] Config: HotkeyTakeoffReady '{}' unparseable - keeping 0x{:02X}"sv,
                              value, cfg.hotkeyTakeoffReady);
                }
            }
            else if (keyLower == "coopquestrewards" || keyLower == "coop_quest_rewards") {
                cfg.coopQuestRewards = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopQuestRewards = {}"sv, cfg.coopQuestRewards);
            }
            else if (keyLower == "coopfriendlyfireguard" || keyLower == "coop_friendly_fire_guard") {
                cfg.coopFriendlyFireGuard = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopFriendlyFireGuard = {}"sv, cfg.coopFriendlyFireGuard);
            }
            else if (keyLower == "coopaimsync" || keyLower == "coop_aim_sync") {
                cfg.coopAimSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopAimSync = {}"sv, cfg.coopAimSync);
            }
            else if (keyLower == "coopjetpacksync" || keyLower == "coop_jetpack_sync") {
                cfg.coopJetpackSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopJetpackSync = {}"sv, cfg.coopJetpackSync);
            }
            else if (keyLower == "coopsharedworlditems" || keyLower == "coop_shared_world_items") {
                cfg.coopSharedWorldItems = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopSharedWorldItems = {}"sv, cfg.coopSharedWorldItems);
            }
            else if (keyLower == "autoreconnect" || keyLower == "auto_reconnect") {
                cfg.autoReconnect = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: AutoReconnect = {}"sv, cfg.autoReconnect);
            }
            else if (keyLower == "cooprestoreowncharacter" || keyLower == "coop_restore_own_character") {
                cfg.coopRestoreOwnCharacter = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopRestoreOwnCharacter = {}"sv, cfg.coopRestoreOwnCharacter);
            }
            else if (keyLower == "coopnpchealthsync" || keyLower == "coop_npc_health_sync") {
                cfg.coopNpcHealthSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopNpcHealthSync = {}"sv, cfg.coopNpcHealthSync);
            }
            else if (keyLower == "coopscenesync" || keyLower == "coop_scene_sync") {
                cfg.coopSceneSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopSceneSync = {}"sv, cfg.coopSceneSync);
            }
            else if (keyLower == "coopqueststartsync" || keyLower == "coop_quest_start_sync") {
                cfg.coopQuestStartSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopQuestStartSync = {}"sv, cfg.coopQuestStartSync);
            }
            else if (keyLower == "coopmissionbridge" || keyLower == "coop_mission_bridge") {
                cfg.coopMissionBridge = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopMissionBridge = {}"sv, cfg.coopMissionBridge);
            }
            else if (keyLower == "coopshiphullsync" || keyLower == "coop_ship_hull_sync") {
                cfg.coopShipHullSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopShipHullSync = {}"sv, cfg.coopShipHullSync);
            }
            else if (keyLower == "coopdocksync" || keyLower == "coop_dock_sync") {
                cfg.coopDockSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopDockSync = {}"sv, cfg.coopDockSync);
            }
            else if (keyLower == "coopleavehostseat" || keyLower == "coop_leave_host_seat") {
                cfg.coopLeaveHostSeat = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopLeaveHostSeat = {}"sv, cfg.coopLeaveHostSeat);
            }
            else if (keyLower == "coopclientseatsync" || keyLower == "coop_client_seat_sync") {
                cfg.coopClientSeatSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopClientSeatSync = {}"sv, cfg.coopClientSeatSync);
            }
            else if (keyLower == "coopshipbodybyorigin" || keyLower == "coop_ship_body_by_origin") {
                cfg.coopShipBodyByOrigin = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopShipBodyByOrigin = {}"sv, cfg.coopShipBodyByOrigin);
            }
            else if (keyLower == "coopspacetimesync" || keyLower == "coop_space_time_sync") {
                cfg.coopSpaceTimeSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopSpaceTimeSync = {}"sv, cfg.coopSpaceTimeSync);
            }
            else if (keyLower == "coopshipvelocitysync" || keyLower == "coop_ship_velocity_sync") {
                cfg.coopShipVelocitySync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopShipVelocitySync = {}"sv, cfg.coopShipVelocitySync);
            }
            else if (keyLower == "coopfaststandup" || keyLower == "coop_fast_stand_up") {
                cfg.coopFastStandUp = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopFastStandUp = {}"sv, cfg.coopFastStandUp);
            }
            else if (keyLower == "coopstanduprespawnms" || keyLower == "coop_stand_up_respawn_ms") {
                try {
                    cfg.coopStandUpRespawnMs = static_cast<std::uint32_t>(std::clamp(std::stoi(value), 0, 5000));
                } catch (...) {}
                PLOG_INFO("[PULSAR] Config: CoopStandUpRespawnMs = {}"sv, cfg.coopStandUpRespawnMs);
            }
            else if (keyLower == "coopdoorfollow" || keyLower == "coop_door_follow") {
                cfg.coopDoorFollow = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopDoorFollow = {}"sv, cfg.coopDoorFollow);
            }
            else if (keyLower == "coopbridgerestoreretry" || keyLower == "coop_bridge_restore_retry") {
                cfg.coopBridgeRestoreRetry = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopBridgeRestoreRetry = {}"sv, cfg.coopBridgeRestoreRetry);
            }
            else if (keyLower == "sharedvehicleride" || keyLower == "shared_vehicle_ride") {
                const auto mode = Pulsar::VehicleSync::ParseRideMode(value);
                cfg.sharedVehicleRide = static_cast<int>(mode);
                PLOG_INFO("[PULSAR] Config: SharedVehicleRide = {}"sv,
                          Pulsar::VehicleSync::RideModeName(mode));
            }
            else if (keyLower == "autoregroupinspace" || keyLower == "auto_regroup_in_space") {
                cfg.autoRegroupInSpace = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: AutoRegroupInSpace = {}"sv, cfg.autoRegroupInSpace);
            }
            else if (keyLower == "coopnomenupause" || keyLower == "coop_no_menu_pause") {
                cfg.coopNoMenuPause = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopNoMenuPause = {}"sv, cfg.coopNoMenuPause);
            }
            else if (keyLower == "cooptimeweathersync" || keyLower == "coop_time_weather_sync") {
                cfg.coopTimeWeatherSync = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopTimeWeatherSync = {}"sv, cfg.coopTimeWeatherSync);
            }
            else if (keyLower == "e2eencryption" || keyLower == "e2e_encryption") {
                cfg.e2eEncryption = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: E2EEncryption = {}"sv, cfg.e2eEncryption);
            }
            else if (keyLower == "e2erelayauth" || keyLower == "e2e_relay_auth") {
                cfg.e2eRelayAuth = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: E2ERelayAuth = {}"sv, cfg.e2eRelayAuth);
            }
            else if (keyLower == "coopplanetfasttravelfollow" || keyLower == "coop_planet_fasttravel_follow") {
                cfg.coopPlanetFastTravelFollow = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopPlanetFastTravelFollow = {}"sv, cfg.coopPlanetFastTravelFollow);
            }
            else if (keyLower == "cloneuseplayerstates" || keyLower == "clone_use_player_states") {
                cfg.cloneUsePlayerStates = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CloneUsePlayerStates = {}"sv, cfg.cloneUsePlayerStates);
            }
            else if (keyLower == "cooptraitnpcsforpartner" ||
                     keyLower == "coop_trait_npcs_for_partner") {
                cfg.coopTraitNpcsForPartner = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopTraitNpcsForPartner = {}"sv, cfg.coopTraitNpcsForPartner);
            }
            else if (keyLower == "cooppartnercountsasfollower" ||
                     keyLower == "coop_partner_counts_as_follower") {
                cfg.coopPartnerCountsAsFollower = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopPartnerCountsAsFollower = {}"sv,
                          cfg.coopPartnerCountsAsFollower);
            }
            else if (keyLower == "coopsharekillxp" || keyLower == "coop_share_kill_xp") {
                cfg.coopShareKillXp = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopShareKillXp = {}"sv, cfg.coopShareKillXp);
            }
            else if (keyLower == "cooptamecombatai" || keyLower == "coop_tame_combat_ai") {
                cfg.coopTameCombatAI = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: CoopTameCombatAI = {}"sv, cfg.coopTameCombatAI);
            }
            else if (keyLower == "traceevents" || keyLower == "trace_events") {
                cfg.traceEvents = ParseBool(value);
                PLOG_INFO("[PULSAR] Config: TraceEvents = {}"sv, cfg.traceEvents);
            }
        }

        return cfg;
    }

    Config LoadConfig()
    {
        Config cfg;

        // Find INI next to the DLL
        char modulePath[MAX_PATH]{};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&LoadConfig), &hModule);
        GetModuleFileNameA(hModule, modulePath, MAX_PATH);

        std::string iniPath(modulePath);
        auto pos = iniPath.rfind('\\');
        if (pos != std::string::npos) {
            iniPath = iniPath.substr(0, pos + 1);
        }
        iniPath += "PULSAR.ini";

        std::ifstream file(iniPath);
        if (!file.is_open()) {
            PLOG_INFO("[PULSAR] No config file at {}, using defaults (Host mode)"sv, iniPath);
            return cfg;
        }

        PLOG_INFO("[PULSAR] Loading config from {}"sv, iniPath);

        return ParseConfig(file);
    }

    std::string GetPluginDir()
    {
        char modulePath[MAX_PATH]{};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&LoadConfig), &hModule);
        GetModuleFileNameA(hModule, modulePath, MAX_PATH);

        std::string dir(modulePath);
        auto pos = dir.rfind('\\');
        if (pos != std::string::npos) {
            dir = dir.substr(0, pos + 1);
        }
        return dir;
    }
}
