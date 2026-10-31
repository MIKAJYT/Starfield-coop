#include "PCH.h"
#include "Debug/TestBot.h"

#ifndef PULSAR_HARDENED

#include "Animation/BoneSync.h"
#include "Config.h"
#include "InventorySync/DirectRestore.h"
#include "NPCSync/NPCSync.h"
#include "Network/NetworkHost.h"
#include "Papyrus/GameCalls.h"
#include "SceneSync/SceneSync.h"
#include "QuestSync/QuestSync.h"
#include "QuestSync/RewardShare.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "SpaceSync/SpaceSync.h"
#include "SpaceSync/FastTravelSync.h"
#include "Coop/CoopMortality.h"
#include "Coop/TakeoffVote.h"
#include "UI/HudMessage.h"
#include "TransitSync/TransitSync.h"
#include "WorldSync/LootSync.h"
#include "WorldSync/WorldSync.h"

#include <RE/B/BGSScene.h>
#include <RE/M/MemoryManager.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <fstream>
#include <string>
#include <algorithm>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::TestBot
{
    namespace
    {
        // Scenarios (tests/coopqa/Run-CoopQA.ps1 -Scenario):
        //   follow - the host moves itself; the client's clone must follow.
        //   door   - the host goes through the nearest load door; with
        //            CoopDoorFollow=0 the client must stay where it is.
        //   loot   - the host kills a nearby NPC and takes everything from the
        //            corpse; with CoopInstancedEnemyLoot=1 the client's copy of
        //            the corpse must keep its items. Both roles report the dead
        //            NPCs around them with their item counts.
        //   reward - the host gets credits and a weapon as quest rewards
        //            (reason kQuestReward, no source); with CoopQuestRewards=1
        //            the client must get both too. The client's own copy of the
        //            quest also gives the weapon (as a replicated stage script
        //            would), so it must end up with one weapon, not two.
        //   friendlyfire - the host hits the client's figure twice, as the
        //            attacker: 60 % of its health, then 4 s later more than all
        //            of it. With CoopFriendlyFireGuard=1 the figure must be back
        //            to full health within 2 s each time and never go down, and
        //            the client's own health must not move.
        //   aim    - the host draws its weapon and aims: 8 s looking up (+0.6 rad)
        //            and aiming, 8 s looking down (-0.6 rad) and aiming, then level
        //            and not aiming. The client reports the pitch it received, its
        //            clone's head bone and whether the clone is in the sights pose;
        //            with CoopAimSync=1 the head must turn between up and down.
        //   jetpack - the host jumps and fires its boostpack for 2.5 s (the engine's
        //            ActionJump, ActionBoostpackStart, ActionBoostpackStop). Both
        //            sides track heights every frame; the client's clone must rise
        //            with CoopJetpackSync=1).
        //   pickup - the host takes the nearest loose item lying in the world (a
        //            save-file reference, so it is the same reference on both
        //            machines). With CoopSharedWorldItems=1 it must be gone on the
        //            client too. Both roles report the loose items around them.
        //   reconnect - the client's network goes dark for 25 s (every received
        //            datagram dropped). The connection must time out and, with
        //            AutoReconnect=1, come back by itself, partner figures and all.
        struct Settings {
            bool        enabled = false;
            std::string stateFile;          // state log, one JSON line per second (test slot logs)
            std::string scenario = "follow";
            std::uint64_t moveAfterMs = 45000;   // host: clone visible this long, then act
            float       moveMetres = 5.0f;  // follow: X offset of the test move (game units = metres)
            float       doorRadius = 60.0f; // door: search radius for a load door (metres)
            float       lootRadius = 30.0f; // loot: search radius for the NPC to kill (metres)
            std::uint32_t sceneId = 0;          // scene: the scene the host starts; 0 = search running Akila scenes
            std::string   checkRefsFile;        // space: reference IDs (hex, one per line) to look up in this game
            std::uint64_t lootAfterDeathMs = 5000;    // loot: wait after the death before taking
            int         rewardCredits = 500;          // reward: credits given as the quest reward
            std::string notifyTest;                   // message check: on-screen text sent 3 times, +0/+6/+12 s after the act
            bool        takeoffPartnerOutside = false;  // takeoff: the client leaves the ship first, so the vote must move it aboard
            bool        takeoffF8DuringVote = false;    // takeoff: the pilot "presses" F8 (regroup key) while the vote waits
            bool        takeoffMenuOnly = false;        // takeoff experiment: open the takeoff menu, never fire the launch
        };

        // Loot scenario progress (host). The kill happens at the usual act time;
        // the take - the action under test, stamped as actedAtWallMs - follows
        // once the corpse has had time to die on the client too.
        enum class LootPhase { kIdle, kKilled, kDone };

        struct State {
            bool          loaded = false;
            Settings      cfg;
            std::uint64_t nextWriteMs = 0;
            std::uint64_t cloneSeenSinceMs = 0;
            std::uint64_t firstCloneSeenMs = 0;   // first tick the clone was visible (never reset)
            bool          partnerExited = false; // takeoff, client: tried the ship hatch
            std::string   partnerExitInfo;
            bool          acted = false;
            std::uint64_t actedAtWallMs = 0;
            std::string   actionInfo;
            int           writes = 0;
            LootPhase     lootPhase = LootPhase::kIdle;
            std::uint32_t lootTarget = 0;
            std::uint64_t lootDeadAtMs = 0;
            std::uint64_t lootKilledAtMs = 0;
            int           lootTries = 0;
            std::uint32_t rewardItem = 0;        // reward: both roles pick it from the same save
            bool          rewardOwnCopy = false; // client: its own quest copy gave the weapon
            int           takeoffStep = 0;       // takeoff: 0 idle, 1 sat down, 2 menu open, 3 launch asked
            std::uint64_t takeoffSatAtMs = 0;
            std::string   takeoffSeatInfo;
            std::uint64_t voteSeenAtMs = 0;      // client: the takeoff request arrived (tick)
            std::uint64_t voteSeenWallMs = 0;
            std::uint64_t voteReadyWallMs = 0;   // client: answered ready
            std::uint64_t f8AtWallMs = 0;        // host: F8 pressed during the vote (TakeoffF8DuringVote)
            int           ffHits = 0;            // friendlyfire, host: hits dealt to the figure
            std::uint64_t ffLastHitMs = 0;
            std::string   ffHitInfo;
            std::uint64_t aimStartMs = 0;        // aim, host: tick the steering began (0 = not yet)
            bool          aimDrawn = false;      // aim, host: weapon drawn before aiming
            int           jetStep = 0;           // jetpack, host: 0 idle, 1 jumped, 2 boosting, 3 stopped
            std::uint64_t jetStepMs = 0;
            std::string   jetInfo;
            float         jetZMin = 1e9f, jetZMax = -1e9f;            // own height since the last sample
            float         jetCloneZMin = 1e9f, jetCloneZMax = -1e9f;  // the clone's
            int           jetJumpMask = 0;       // own iSyncJumpState values seen since the last sample (bit per value)
            std::uint32_t pickRef = 0;           // pickup, host: the item taken
            // separate: each player changes its own character, then the host bridges.
            std::uint32_t sepItemH = 0, sepItemC = 0;   // host's item (MISC), client's item (AMMO)
            std::int32_t  sepBaseH = -1, sepBaseC = -1; // their counts before
            std::uint32_t sepBaseSp = 0;                // own skill points before
            std::uint32_t sepPerk = 0;                  // own perk ranked up
            std::int32_t  sepPerkBefore = -1, sepPerkAfter = -1;
            std::uint32_t sepRestoresAtAct = 0;
            std::uint64_t sepLoadAtAct = 0;             // last save load (tick) when acting
            std::vector<std::uint32_t> sceneCands;      // scene (host): scenes to try, in order
            bool          sceneSearched = false;
            std::size_t   sceneIdx = 0;                 // the candidate being tried
            std::uint64_t sceneTryMs = 0;               // tick of the last start call
            std::uint64_t sceneTryWallMs = 0;
            std::string   sceneSearchInfo;
            std::uint32_t missionId = 0;                // mission: the story-manager quest under test
            bool          missionPicked = false;
            bool          missionStopped = false;       // client: stopped it quietly
            std::uint64_t missionStoppedWallMs = 0;
            std::uint64_t missionPickedMs = 0;
            std::uint64_t missionRefreshMs = 0;
            std::uint64_t missionLoadAtPick = 0;        // last save load (tick) when picking
            std::uint64_t spaceSinceMs = 0;             // space (host): first tick in space
            int           bridgesFired = 0;             // bridges (host): save bridges sent
            std::string   bridgesFiredAt;               //   their wall times, comma-separated
            std::uint64_t bridgeNextMs = 0;
            int           bridgeLoads = 0;              // bridges (client): saves loaded
            std::string   bridgeLoadsAt;
            std::uint64_t bridgeLastLoad = 0;
            std::uint64_t travelAtWallMs = 0;           // space (host): ship moved to the station
            std::string   travelInfo;
            std::uint64_t sepBridgeWallMs = 0;          // host: bridge fired
            std::uint64_t sepActMs = 0;
            int           sepBridgeTries = 0;
            // damage: host hits one NPC, the client another.
            std::uint32_t dmgTarget = 0;
            std::uint64_t dmgHitWallMs = 0;
            float         dmgBefore = -1.0f, dmgAfter = -1.0f;
            int           notifySent = 0;        // message check: test messages sent
        };
        State s_state;

        // "space" flies the takeoff scenario, then writes a census of the space
        // around the ship (phase 5: stations and encounters differ between players).
        bool TakeoffLike()
        {
            return s_state.cfg.scenario == "takeoff" || s_state.cfg.scenario == "space";
        }

        std::uint64_t WallMs()
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        std::string Trim(std::string a_s)
        {
            while (!a_s.empty() && (a_s.back() == ' ' || a_s.back() == '\t' || a_s.back() == '\r')) a_s.pop_back();
            std::size_t i = 0;
            while (i < a_s.size() && (a_s[i] == ' ' || a_s[i] == '\t')) ++i;
            return a_s.substr(i);
        }

        Settings LoadSettings()
        {
            Settings s;
            std::ifstream in(Pulsar::GetPluginDir() + "PulsarTestBot.ini");
            if (!in) return s;
            std::string line;
            while (std::getline(in, line)) {
                const auto semi = line.find(';');
                if (semi != std::string::npos) line.resize(semi);
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = Trim(line.substr(0, eq));
                const std::string value = Trim(line.substr(eq + 1));
                for (auto& c : key) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                if (key == "enabled")          s.enabled = (value == "1" || value == "true");
                else if (key == "statefile")   s.stateFile = value;
                else if (key == "moveafterms") s.moveAfterMs = std::stoull(value);
                else if (key == "movemetres")  s.moveMetres = std::stof(value);
                else if (key == "scenario")    s.scenario = value;
                else if (key == "doorradius")  s.doorRadius = std::stof(value);
                else if (key == "lootradius")  s.lootRadius = std::stof(value);
                else if (key == "sceneid")     s.sceneId = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16));
                else if (key == "checkrefsfile") s.checkRefsFile = value;
                else if (key == "lootafterdeathms") s.lootAfterDeathMs = std::stoull(value);
                else if (key == "rewardcredits") s.rewardCredits = std::stoi(value);
                else if (key == "notifytest") s.notifyTest = value;
                else if (key == "takeoffpartneroutside") s.takeoffPartnerOutside = (value == "1" || value == "true");
                else if (key == "takeofff8duringvote") s.takeoffF8DuringVote = (value == "1" || value == "true");
                else if (key == "takeoffmenuonly") s.takeoffMenuOnly = (value == "1" || value == "true");
            }
            if (s.stateFile.empty()) s.enabled = false;
            return s;
        }

        // SEH-wrapped raw reads (C2712: no unwinding objects inside __try).
        // Same offsets as CellDiag: data.location at +0x8C, loadedData at +0xB8,
        // its data3D at +0x08.
        struct ActorSnap {
            bool          valid = false;
            std::uint32_t cellID = 0;
            float         x = 0, y = 0, z = 0;
            bool          has3D = false;
        };

        ActorSnap Snap(RE::Actor* a_actor) noexcept
        {
            ActorSnap s{};
            __try {
                if (!a_actor) return s;
                auto* base = reinterpret_cast<std::uint8_t*>(a_actor);
                if (a_actor->parentCell) s.cellID = a_actor->parentCell->formID;
                auto* loc = reinterpret_cast<float*>(base + 0x8C);
                s.x = loc[0]; s.y = loc[1]; s.z = loc[2];
                const auto loaded = *reinterpret_cast<std::uintptr_t*>(base + 0xB8);
                s.has3D = loaded && *reinterpret_cast<std::uintptr_t*>(loaded + 0x08);
                s.valid = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return s;
        }

        // Nearest load door (a DOOR reference with an ExtraTeleport destination)
        // in the player's cell, within a_radius. Raw, SEH-wrapped walk of the
        // cell's reference array, the layout VehicleSync's FURN scan uses
        // (cell+0x80 size, +0x88 data; ref+0x98 base form, base+0x2E form type).
        // Per-door evidence when no load door is found (PULSAR.log only).
        struct DoorSeen {
            RE::TESObjectREFR* ref;
            std::uint32_t      baseID;
            float              dist;
        };

        void LogDoorsInCell(RE::TESObjectCELL* a_cell, float a_px, float a_py, float a_pz)
        {
            DoorSeen seen[64];
            int n = 0;
            [&]() noexcept {
                __try {
                    if (!a_cell) return;
                    auto* cb = reinterpret_cast<std::uint8_t*>(a_cell);
                    const std::uint32_t size = *reinterpret_cast<std::uint32_t*>(cb + 0x80);
                    auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                    if (!data || size == 0 || size > 20000) return;
                    for (std::uint32_t i = 0; i < size && n < 64; ++i) {
                        auto* ref = data[i];
                        if (!ref) continue;
                        auto* base = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(ref) + 0x98);
                        if (!base || *(base + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kDOOR)) continue;
                        auto* loc = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ref) + 0x8C);
                        const float dx = loc[0] - a_px, dy = loc[1] - a_py, dz = loc[2] - a_pz;
                        seen[n++] = { ref, *reinterpret_cast<std::uint32_t*>(base + 0x28), std::sqrt(dx * dx + dy * dy + dz * dz) };
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }();
            for (int i = 0; i < n; ++i) {
                const char* why = "?";
                Pulsar::TransitSync::GetDoorTeleportDestination(seen[i].ref, why);
                auto* baseForm = seen[i].ref->data.objectReference.get();
                const char* edid = baseForm ? baseForm->GetFormEditorID() : nullptr;
                char line[200];
                std::snprintf(line, sizeof(line), "door ref %08X base %08X '%s' %.1f m teleport=%s",
                              seen[i].ref->formID, seen[i].baseID, edid ? edid : "", seen[i].dist, why);
                const std::string s(line);
                PLOG_INFO("[PULSAR] TestBot: {}"sv, s);
            }
        }

        // Takeoff probe: every furniture reference within a_radius, with its
        // display name (the pilot seat is among them aboard a ship).
        void Trace(const char* a_line);

        int CollectFurniture(RE::TESObjectCELL* a_cell, float a_px, float a_py, float a_pz, float a_radius,
                             DoorSeen* a_out, int a_max);

        // The pilot seat aboard the player's ship: furniture named "Pilot..."
        // ("Pilotensitz" in the German test install, "Pilot Seat" in English).
        RE::TESObjectREFR* FindPilotSeat(RE::TESObjectCELL* a_cell, float a_px, float a_py, float a_pz)
        {
            DoorSeen seen[64];
            const int n = CollectFurniture(a_cell, a_px, a_py, a_pz, 60.0f, seen, 64);
            RE::TESObjectREFR* best = nullptr;
            float bestD = 1e9f;
            for (int i = 0; i < n; ++i) {
                const char* name = seen[i].ref->GetDisplayFullName();
                if (!name || !std::strstr(name, "ilot")) continue;
                if (seen[i].dist < bestD) { best = seen[i].ref; bestD = seen[i].dist; }
            }
            return best;
        }

        void LogFurnitureInCell(RE::TESObjectCELL* a_cell, float a_px, float a_py, float a_pz, float a_radius)
        {
            DoorSeen seen[64];
            const int n = CollectFurniture(a_cell, a_px, a_py, a_pz, a_radius, seen, 64);
            for (int i = 0; i < n; ++i) {
                const char* name = seen[i].ref->GetDisplayFullName();
                char line[200];
                std::snprintf(line, sizeof(line), "furniture ref %08X base %08X '%s' %.1f m",
                              seen[i].ref->formID, seen[i].baseID, name ? name : "", seen[i].dist);
                Trace(line);
                PLOG_INFO("[PULSAR] TestBot: {}"sv, std::string(line));
            }
        }

        int CollectFurniture(RE::TESObjectCELL* a_cell, float a_px, float a_py, float a_pz, float a_radius,
                             DoorSeen* a_out, int a_max)
        {
            DoorSeen* seen = a_out;
            int n = 0;
            [&]() noexcept {
                __try {
                    if (!a_cell) return;
                    auto* cb = reinterpret_cast<std::uint8_t*>(a_cell);
                    const std::uint32_t size = *reinterpret_cast<std::uint32_t*>(cb + 0x80);
                    auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                    if (!data || size == 0 || size > 20000) return;
                    for (std::uint32_t i = 0; i < size && n < a_max; ++i) {
                        auto* ref = data[i];
                        if (!ref) continue;
                        auto* base = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(ref) + 0x98);
                        if (!base || *(base + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kFURN)) continue;
                        auto* loc = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ref) + 0x8C);
                        const float dx = loc[0] - a_px, dy = loc[1] - a_py, dz = loc[2] - a_pz;
                        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d > a_radius) continue;
                        seen[n++] = { ref, *reinterpret_cast<std::uint32_t*>(base + 0x28), d };
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }();
            return n;
        }

        // The player's ship state, for the takeoff scenario's state lines.
        struct ShipSnap {
            bool          inSpace = false;
            std::uint32_t ship = 0;     // GetSpaceship: the ship the player is aboard
            bool          seated = false;
        };

        ShipSnap SnapShip(RE::PlayerCharacter* a_player) noexcept
        {
            ShipSnap s{};
            __try {
                if (!a_player) return s;
                s.inSpace = a_player->IsInSpace(true);
                auto* ship = a_player->GetSpaceship(true);
                s.ship = ship ? ship->formID : 0;
                // Seated: process +0x228, middle-high +0x08, furniture handle +0x428
                // (as SpaceSync's HostSeatedInShip).
                auto* proc = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(a_player) + 0x228);
                auto* mh = proc ? *reinterpret_cast<std::uint8_t**>(proc + 0x08) : nullptr;
                s.seated = mh && *reinterpret_cast<std::uint32_t*>(mh + 0x428) != 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return s;
        }

        // Space census: every reference in the space cell around the ship the
        // player is aboard (stations, other ships, encounters), so the launcher
        // can compare what each player has around them. Same cell layout as
        // SafeListNpcs; distances in metres from the player's ship.
        struct SpaceRef {
            std::uint32_t id;
            std::uint32_t base;
            std::uint8_t  type;     // base form type
            float         dist;
        };

        int SafeSpaceCensus(RE::PlayerCharacter* a_player, std::uint32_t& a_cell, SpaceRef* a_out, int a_max) noexcept
        {
            int n = 0;
            a_cell = 0;
            __try {
                if (!a_player) return 0;
                auto* ship = a_player->GetSpaceship(true);
                if (!ship) return 0;
                auto* cell = ship->parentCell;
                if (!cell) return 0;
                a_cell = cell->formID;
                auto* sb = reinterpret_cast<std::uint8_t*>(ship);
                const auto* sl = reinterpret_cast<float*>(sb + 0x8C);
                auto* cb = reinterpret_cast<std::uint8_t*>(cell);
                const std::uint32_t size = *reinterpret_cast<std::uint32_t*>(cb + 0x80);
                auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                if (!data || size == 0 || size > 20000) return 0;
                for (std::uint32_t i = 0; i < size && n < a_max; ++i) {
                    auto* ref = data[i];
                    if (!ref || ref == ship) continue;
                    auto* rb = reinterpret_cast<std::uint8_t*>(ref);
                    auto* base = *reinterpret_cast<std::uint8_t**>(rb + 0x98);
                    if (!base) continue;
                    const auto* loc = reinterpret_cast<float*>(rb + 0x8C);
                    const float dx = loc[0] - sl[0], dy = loc[1] - sl[1], dz = loc[2] - sl[2];
                    a_out[n++] = { ref->formID, reinterpret_cast<RE::TESForm*>(base)->formID, *(base + 0x2E),
                                   std::sqrt(dx * dx + dy * dy + dz * dz) };
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return n;
        }

        // Space: one reference's state in this game. Same raw layout as
        // SafeListLooseItems: flags +0x20 (bit 11 disabled), parentCell +0xB0,
        // loadedData +0xB8 with its 3D at +0x08.
        struct RefStatus {
            bool          exists = false;
            std::uint32_t cell = 0;
            bool          has3D = false;
            bool          disabled = false;
        };

        RefStatus SafeRefStatus(std::uint32_t a_id) noexcept
        {
            RefStatus s{};
            __try {
                auto* f = RE::TESForm::LookupByID(a_id);
                if (!f) return s;
                auto* rb = reinterpret_cast<std::uint8_t*>(f);
                if (*(rb + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kREFR)) return s;
                s.exists = true;
                s.disabled = (*reinterpret_cast<std::uint32_t*>(rb + 0x20) & (1u << 11)) != 0;
                auto* cell = *reinterpret_cast<std::uint8_t**>(rb + 0xB0);
                s.cell = cell ? *reinterpret_cast<std::uint32_t*>(cell + 0x28) : 0;
                const auto loaded = *reinterpret_cast<std::uintptr_t*>(rb + 0xB8);
                s.has3D = loaded && *reinterpret_cast<std::uintptr_t*>(loaded + 0x08);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return s;
        }

        // The host's station references (CheckRefsFile, from a host census at The
        // Eye), looked up here when a_fresh: do they exist, sit in the ship's space
        // cell, have 3D? Answers whether the station is really missing or only absent
        // from the cell's reference list. Returns the last result as a JSON member.
        std::string RefCheckJson(RE::PlayerCharacter* a_player, bool a_fresh)
        {
            static std::vector<std::uint32_t> s_checkIds;
            static bool s_checkLoaded = false;
            static std::string s_check;
            if (!s_checkLoaded && !s_state.cfg.checkRefsFile.empty()) {
                s_checkLoaded = true;
                std::ifstream in(s_state.cfg.checkRefsFile);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.size() >= 8) s_checkIds.push_back(static_cast<std::uint32_t>(std::stoul(line.substr(0, 8), nullptr, 16)));
                }
                PLOG_INFO("[PULSAR] TestBot: {} references to check"sv, s_checkIds.size());
            }
            // The ship's cell through the SEH-guarded census (no references kept):
            // GetSpaceship outside it crashed the game at the main menu (run
            // 20261031-101354, access violation in Starfield.exe+0x2116846).
            std::uint32_t shipCell = 0;
            if (a_fresh && !s_checkIds.empty()) SafeSpaceCensus(a_player, shipCell, nullptr, 0);
            if (a_fresh && shipCell) {
                int exists = 0, inCell = 0, has3D = 0, has3DInCell = 0, disabled = 0;
                std::uint32_t otherCell = 0;
                for (const auto id : s_checkIds) {
                    const RefStatus st = SafeRefStatus(id);
                    if (!st.exists) continue;
                    ++exists;
                    if (st.disabled) ++disabled;
                    if (st.has3D) ++has3D;
                    if (st.cell == shipCell) { ++inCell; if (st.has3D) ++has3DInCell; }
                    else if (!otherCell) otherCell = st.cell;
                }
                char c[260];
                std::snprintf(c, sizeof(c),
                              ",\"refCheck\":{\"n\":%zu,\"shipCell\":\"%08X\",\"exists\":%d,\"inCell\":%d,\"has3D\":%d,\"has3DInCell\":%d,\"disabled\":%d,\"otherCell\":\"%08X\"}",
                              s_checkIds.size(), shipCell, exists, inCell, has3D, has3DInCell, disabled, otherCell);
                s_check = c;
            }
            return s_check;
        }

        // References whose editor ID contains a_hint (lower case), Starfield.esm IDs,
        // at most a_max, with their names. Reference editor IDs are kept at runtime
        // (EventTrace names MariaStarrettREF).
        std::vector<std::pair<std::uint32_t, std::string>> RefsByName(const char* a_hint, std::size_t a_max)
        {
            std::vector<std::pair<std::uint32_t, std::string>> out;
            for (std::uint32_t id = 0x800; id < 0x00400000 && out.size() < a_max; ++id) {
                auto* f = RE::TESForm::LookupByID(id);
                if (!f || f->GetFormType() != RE::FormType::kREFR) continue;
                const char* eid = f->GetFormEditorID();
                if (!eid || !eid[0]) continue;
                std::string low = eid;
                for (auto& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                if (low.find(a_hint) != std::string::npos) out.emplace_back(id, eid);
            }
            return out;
        }

        // What the door scan saw, for the state log when no door is used.
        struct DoorScan {
            std::uint32_t refs = 0;           // references in the cell array
            int           doors = 0;          // DOOR references
            int           loadDoors = 0;      // ... with a teleport destination
            float         nearestLoadDoor = -1.0f;   // metres, any distance
            bool          seh = false;
        };

        // Load doors within a_radius, nearest first (up to a_max). Some are
        // locked: Akila City's nearest (00297D8D, 2.8 m) is, and the engine
        // refuses it, so the bot tries them in order.
        int SafeFindLoadDoors(RE::TESObjectCELL* a_cell, float a_px, float a_py, float a_pz, float a_radius,
                              DoorScan& a_scan, RE::TESObjectREFR** a_out, float* a_outDist, int a_max) noexcept
        {
            a_scan = {};
            int n = 0;
            __try {
                if (!a_cell) return 0;
                auto* cb = reinterpret_cast<std::uint8_t*>(a_cell);
                const std::uint32_t size = *reinterpret_cast<std::uint32_t*>(cb + 0x80);
                auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                a_scan.refs = size;
                if (!data || size == 0 || size > 20000) return 0;
                const float maxD2 = a_radius * a_radius;
                float nearestD2 = -1.0f;
                for (std::uint32_t i = 0; i < size; ++i) {
                    auto* ref = data[i];
                    if (!ref) continue;
                    auto* base = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(ref) + 0x98);
                    if (!base || *(base + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kDOOR)) continue;
                    ++a_scan.doors;
                    // A load door has teleport data. Its destination may be empty:
                    // the ship hatch in the test save (base 001FCA00) reports
                    // "no-destRef" - the destination is set at run time, where the
                    // ship is. Only "no-extraTeleport" marks an ordinary door.
                    const char* why = "";
                    Pulsar::TransitSync::GetDoorTeleportDestination(ref, why);
                    if (std::strcmp(why, "OK") != 0 && std::strcmp(why, "no-destRef") != 0) continue;
                    ++a_scan.loadDoors;
                    auto* loc = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ref) + 0x8C);
                    const float dx = loc[0] - a_px, dy = loc[1] - a_py, dz = loc[2] - a_pz;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (nearestD2 < 0 || d2 < nearestD2) nearestD2 = d2;
                    if (d2 >= maxD2) continue;
                    // Insertion into the sorted list; drop the farthest when full.
                    int at = n;
                    while (at > 0 && a_outDist[at - 1] > d2) --at;
                    if (at >= a_max) continue;
                    for (int k = (n < a_max ? n : a_max - 1); k > at; --k) {
                        a_out[k] = a_out[k - 1];
                        a_outDist[k] = a_outDist[k - 1];
                    }
                    a_out[at] = ref;
                    a_outDist[at] = d2;
                    if (n < a_max) ++n;
                }
                if (nearestD2 >= 0) a_scan.nearestLoadDoor = std::sqrt(nearestD2);
                for (int k = 0; k < n; ++k) a_outDist[k] = std::sqrt(a_outDist[k]);
                return n;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                a_scan.seh = true;
                return 0;
            }
        }

        // NPC actors in a cell (raw cell array, as the door scan). Only save-file
        // references (formID below 0xFF000000): those are the same reference on
        // both machines, so the client can report the host's victim by its ID.
        struct NpcSeen {
            RE::Actor*    actor;
            std::uint32_t id;
            float         dist;
            bool          dead;
            bool          guarded;   // Essential or Protected (Actor::boolFlags +0x378): the player cannot kill it
        };

        bool SafeIsDead(RE::Actor* a_actor) noexcept
        {
            __try {
                return a_actor && a_actor->IsDead();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        int SafeListNpcs(RE::TESObjectCELL* a_cell, float a_px, float a_py, float a_pz, float a_radius,
                         NpcSeen* a_out, int a_max) noexcept
        {
            int n = 0;
            __try {
                if (!a_cell) return 0;
                auto* cb = reinterpret_cast<std::uint8_t*>(a_cell);
                const std::uint32_t size = *reinterpret_cast<std::uint32_t*>(cb + 0x80);
                auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                if (!data || size == 0 || size > 20000) return 0;
                const float maxD2 = a_radius * a_radius;
                for (std::uint32_t i = 0; i < size && n < a_max; ++i) {
                    auto* ref = data[i];
                    if (!ref) continue;
                    auto* rb = reinterpret_cast<std::uint8_t*>(ref);
                    if (*(rb + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kACHR)) continue;
                    auto* base = *reinterpret_cast<std::uint8_t**>(rb + 0x98);
                    if (!base || *(base + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kNPC_)) continue;
                    const std::uint32_t id = ref->formID;
                    if (id == 0x14 || id >= 0xFF000000u) continue;
                    auto* loc = reinterpret_cast<float*>(rb + 0x8C);
                    const float dx = loc[0] - a_px, dy = loc[1] - a_py, dz = loc[2] - a_pz;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 >= maxD2) continue;
                    auto* actor = static_cast<RE::Actor*>(ref);
                    const std::uint32_t flags = *reinterpret_cast<std::uint32_t*>(rb + 0x378);
                    const bool guarded = (flags & ((1u << 18) | (1u << 19))) != 0;   // kEssential, kProtected
                    a_out[n++] = { actor, id, std::sqrt(d2), SafeIsDead(actor), guarded };
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return n;
        }

        // Loose items lying in the world (loot form types, the ones LootSync mirrors),
        // save-file references only, within a_radius.
        struct ItemSeen {
            RE::TESObjectREFR* ref;
            std::uint32_t      id;
            float              dist;
            bool               present;   // enabled, not deleted, 3D loaded
        };

        int SafeListLooseItems(RE::TESObjectCELL* a_cell, float a_px, float a_py, float a_pz, float a_radius,
                               ItemSeen* a_out, int a_max) noexcept
        {
            int n = 0;
            __try {
                if (!a_cell) return 0;
                auto* cb = reinterpret_cast<std::uint8_t*>(a_cell);
                const std::uint32_t size = *reinterpret_cast<std::uint32_t*>(cb + 0x80);
                auto** data = *reinterpret_cast<RE::TESObjectREFR***>(cb + 0x88);
                if (!data || size == 0 || size > 20000) return 0;
                const float maxD2 = a_radius * a_radius;
                for (std::uint32_t i = 0; i < size && n < a_max; ++i) {
                    auto* ref = data[i];
                    if (!ref) continue;
                    auto* rb = reinterpret_cast<std::uint8_t*>(ref);
                    auto* base = *reinterpret_cast<std::uint8_t**>(rb + 0x98);
                    if (!base) continue;
                    switch (static_cast<RE::FormType>(*(base + 0x2E))) {
                    case RE::FormType::kARMO: case RE::FormType::kBOOK: case RE::FormType::kMISC:
                    case RE::FormType::kWEAP: case RE::FormType::kAMMO: case RE::FormType::kALCH:
                    case RE::FormType::kINGR: case RE::FormType::kNOTE:
                        break;
                    default:
                        continue;
                    }
                    const std::uint32_t id = ref->formID;
                    if (id >= 0xFF000000u) continue;
                    auto* loc = reinterpret_cast<float*>(rb + 0x8C);
                    const float dx = loc[0] - a_px, dy = loc[1] - a_py, dz = loc[2] - a_pz;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 >= maxD2) continue;
                    const std::uint32_t flags = *reinterpret_cast<std::uint32_t*>(rb + 0x20);
                    const auto loaded = *reinterpret_cast<std::uintptr_t*>(rb + 0xB8);
                    const bool has3D = loaded && *reinterpret_cast<std::uintptr_t*>(loaded + 0x08);
                    const bool present = !(flags & (1u << 11)) && !(flags & (1u << 5)) && has3D;   // kDisabled, kDeleted
                    a_out[n++] = { ref, id, std::sqrt(d2), present };
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return n;
        }

        // Activate a door as the player would, through the engine's Activate
        // core (REL::ID 63380): the function the Papyrus ObjectReference.Activate
        // native calls and LootSync hooks, so every hook sees a real player
        // activation. Tried before and dropped:
        //  - TESObjectREFR::Activate(OBJ_ACTIVATE_DATA) returned false for every
        //    load door (ship hatch, Akila City door 00297D8D) and nobody moved.
        //  - Papyrus ObjectReference.Activate via DispatchMethodCall took the host
        //    down (run 20261027-231108): its game thread went silent the moment
        //    the bot acted and no F.8 door probe was logged, so Activate was
        //    never reached.
        // Deliberately NOT TransitSync's SafeActivateDoor: that marks the
        // activation as an auto-follow and hides the transit under test.
        struct ActivateArgs {
            RE::TESObjectREFR* activator;
            std::uint64_t      unused1;
            std::uint64_t      unused2;
            std::uint32_t      countOrFlag;
            std::uint8_t       flag1c;   // abDefaultProcessingOnly
            std::uint8_t       flag1d;
            std::uint8_t       flag1e;
            std::uint8_t       flag1f;
        };
        static_assert(sizeof(ActivateArgs) == 0x20);

        using ActivateFn = bool(__fastcall*)(RE::TESObjectREFR*, ActivateArgs*);

        ActivateFn EngineActivate()
        {
            static const auto s_fn = reinterpret_cast<ActivateFn>(REL::Relocation<std::uintptr_t>{ REL::ID(63380) }.address());
            return s_fn;
        }

        // 1 = activated, 0 = engine refused, -1 = exception.
        int SafeActivateAsPlayer(RE::TESObjectREFR* a_door, RE::PlayerCharacter* a_player) noexcept
        {
            if (!a_door || !a_player) return 0;
            const ActivateFn s_activate = EngineActivate();
            ActivateArgs args{};
            args.activator   = a_player;
            args.countOrFlag = 1;
            args.flag1d      = 1;
            __try {
                return s_activate(a_door, &args) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // Breadcrumbs that survive a dead game thread: one flushed line per
        // step of the bot's action, next to the state log.
        void Trace(const char* a_line)
        {
            static std::FILE* s_file = nullptr;
            if (!s_file) {
                std::string path = s_state.cfg.stateFile;
                const auto slash = path.find_last_of("\\/");
                path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "bot-trace.log";
                s_file = _fsopen(path.c_str(), "a", _SH_DENYWR);
                if (!s_file) return;
            }
            std::fprintf(s_file, "%llu %s\n", static_cast<unsigned long long>(WallMs()), a_line);
            std::fflush(s_file);
        }

        constexpr std::uint32_t kCredits = 0x0000000Fu;

        RE::ActorValueInfo* HealthAV()
        {
            static RE::ActorValueInfo* s_av = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("Health");
            return s_av;
        }

        float SafeHealth(RE::Actor* a_actor) noexcept
        {
            auto* av = HealthAV();
            __try {
                return a_actor && av ? a_actor->GetActorValue(*av) : -1.0f;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1.0f;
            }
        }

        // A hit's health loss, with the player as the source (the damage modifier a
        // weapon hit adds). Returns the health before the hit, -1 on failure.
        bool SafeDrawWeapon(RE::PlayerCharacter* a_player) noexcept
        {
            __try {
                if (!a_player) return false;
                a_player->DrawWeaponMagicHands(true);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The clone's head bone (C_Head) world rotation, row by row. The bone cache is
        // resolved again for every sample: a respawned clone has a new skeleton.
        bool SafeHeadRotation(RE::Actor* a_actor, float (&a_out)[9]) noexcept
        {
            static Pulsar::BoneSync::BoneCache s_cache;
            RE::NiNode* root = nullptr;
            __try {
                if (!a_actor) return false;
                const auto loaded = *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uint8_t*>(a_actor) + 0xB8);
                auto* obj = loaded ? *reinterpret_cast<RE::NiAVObject**>(loaded + 0x08) : nullptr;
                root = obj ? obj->GetAsNiNode() : nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
            if (!root) return false;
            s_cache = {};
            s_cache.Resolve(root);
            __try {
                auto* head = s_cache.bones[6];
                if (!head) return false;
                const auto& m = head->world.rotate;
                for (int r = 0; r < 3; ++r) {
                    a_out[r * 3 + 0] = m.entry[r].x;
                    a_out[r * 3 + 1] = m.entry[r].y;
                    a_out[r * 3 + 2] = m.entry[r].z;
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        int SafeGraphInt(RE::Actor* a_actor, const char* a_name)
        {
            if (!a_actor) return -1;
            RE::BSFixedString var(a_name);
            std::int32_t v = -1;
            a_actor->GetGraphVariableImplInt(var, v);
            return v;
        }

        float SafeHitAsPlayer(RE::Actor* a_target, RE::PlayerCharacter* a_player, float a_fraction) noexcept
        {
            auto* av = HealthAV();
            __try {
                if (!a_target || !av) return -1.0f;
                const float before = a_target->GetActorValue(*av);
                a_target->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *av, -(before * a_fraction + 1.0f), a_player);
                return before;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1.0f;
            }
        }

        // Give the player an item the way a quest script does: no source
        // container, reason kQuestReward. Returns false if the form is unusable.
        bool GiveAsQuestReward(RE::PlayerCharacter* a_player, std::uint32_t a_base, std::int32_t a_count)
        {
            auto* form = RE::TESForm::LookupByID(a_base);
            if (!form || !form->IsBoundObject()) return false;
            a_player->AddObjectToContainer(reinterpret_cast<RE::TESBoundObject*>(form), nullptr, a_count,
                                           nullptr, RE::ITEM_TRANSFER_REASON::kQuestReward);
            return true;
        }

        // Separate characters: add items without any co-op sync seeing them (the
        // replay guard LootSync honours for network-applied items).
        bool AddOwnItem(RE::PlayerCharacter* a_player, std::uint32_t a_base, std::int32_t a_count)
        {
            auto* form = RE::TESForm::LookupByID(a_base);
            if (!form || !form->IsBoundObject()) return false;
            const bool prev = Pulsar::WorldSync::Loot::SetReplayingFromPacket(true);
            a_player->AddObjectToContainer(reinterpret_cast<RE::TESBoundObject*>(form), nullptr, a_count,
                                           nullptr, RE::ITEM_TRANSFER_REASON::kNone);
            Pulsar::WorldSync::Loot::SetReplayingFromPacket(prev);
            return true;
        }

        // Separate characters: both players pick the same two items from the same
        // save (host's item: lowest MISC form, client's: lowest AMMO form). Each
        // then adds to its own item, one rank to one of its own perks (host: from
        // the lowest form ID up, client: from the highest down) and skill points.
        void ActSeparate(RE::PlayerCharacter* a_player, bool a_isHost)
        {
            namespace DR = Pulsar::InventorySync::DirectRestore;
            s_state.sepItemH = Pulsar::WorldSync::Loot::LowestItemOfTypeForTest(a_player, static_cast<std::uint8_t>(RE::FormType::kMISC));
            s_state.sepItemC = Pulsar::WorldSync::Loot::LowestItemOfTypeForTest(a_player, static_cast<std::uint8_t>(RE::FormType::kAMMO));
            s_state.sepBaseH = s_state.sepItemH ? Pulsar::WorldSync::Loot::CountItemForTest(a_player, s_state.sepItemH) : -1;
            s_state.sepBaseC = s_state.sepItemC ? Pulsar::WorldSync::Loot::CountItemForTest(a_player, s_state.sepItemC) : -1;
            DR::SkillPointsForTest(s_state.sepBaseSp);
            s_state.sepRestoresAtAct = DR::RestoresForTest();
            s_state.sepLoadAtAct = Pulsar::WorldSync::GetLastLoadCompletedTick();

            const std::uint32_t item = a_isHost ? s_state.sepItemH : s_state.sepItemC;
            const std::int32_t add = a_isHost ? 3 : 5;
            const bool itemOk = item && AddOwnItem(a_player, item, add);

            std::vector<std::pair<std::uint32_t, std::int8_t>> perks;
            DR::PerksForTest(perks);
            std::sort(perks.begin(), perks.end());
            if (!a_isHost) std::reverse(perks.begin(), perks.end());
            // Both sides walk the whole list from opposite ends (the top ranks of
            // the QA save are maxed, run 20261030-205404: 12 of 12 failed); the
            // scorer rejects a run where they met on the same perk.
            int tried = 0;
            for (const auto& pk : perks) {
                ++tried;
                const std::int32_t before = DR::PerkRankForTest(pk.first);
                DR::AddPerkRankForTest(pk.first);
                const std::int32_t after = DR::PerkRankForTest(pk.first);
                if (after > before) {
                    s_state.sepPerk = pk.first;
                    s_state.sepPerkBefore = before;
                    s_state.sepPerkAfter = after;
                    break;
                }
            }
            const std::uint32_t spAdd = a_isHost ? 3 : 1;
            const bool spOk = DR::SetSkillPointsForTest(s_state.sepBaseSp + spAdd);

            char msg[260];
            std::snprintf(msg, sizeof(msg),
                          "separate: item %08X +%d ok=%d (host item %08X base %d, client item %08X base %d); perk %08X rank %d -> %d (%d of %zu tried); skill points %u +%u ok=%d",
                          item, add, itemOk ? 1 : 0, s_state.sepItemH, s_state.sepBaseH, s_state.sepItemC, s_state.sepBaseC,
                          s_state.sepPerk, s_state.sepPerkBefore, s_state.sepPerkAfter, tried, perks.size(),
                          s_state.sepBaseSp, spAdd, spOk ? 1 : 0);
            s_state.actionInfo = msg;
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            s_state.sepActMs = ::GetTickCount64();
            Trace(msg);
            PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
        }

        // Damage: hit the chosen NPC by a fraction of its health. The NPC itself is
        // the source (a hit by the player is a crime in a city, see TickLoot).
        float SafeHitBySelf(RE::Actor* a_target, float a_fraction) noexcept
        {
            auto* av = HealthAV();
            __try {
                if (!a_target || !av) return -1.0f;
                const float before = a_target->GetActorValue(*av);
                a_target->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *av, -(before * a_fraction), a_target);
                return before;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1.0f;
            }
        }

        // Damage: host hits the living, health-synced save-file NPC with the lowest
        // form ID nearby, the client the one with the highest (a different NPC when
        // two are around), so each direction has its own victim. Only NPCs whose
        // health the plugin sends count (city civilians send just their position);
        // essential and protected ones are fine, as a 30-40% hit kills nobody.
        void ActDamage(RE::PlayerCharacter* a_player, const ActorSnap& a_me, bool a_isHost)
        {
            NpcSeen npcs[64];
            const int n = SafeListNpcs(a_player->parentCell, a_me.x, a_me.y, a_me.z, s_state.cfg.lootRadius, npcs, 64);
            const NpcSeen* pick = nullptr;
            int alive = 0;
            for (int i = 0; i < n; ++i) {
                if (npcs[i].dead || SafeHealth(npcs[i].actor) <= 1.0f) continue;
                if (!Pulsar::NPCSync::IsHealthSyncedForTest(npcs[i].id)) continue;
                ++alive;
                if (!pick || (a_isHost ? npcs[i].id < pick->id : npcs[i].id > pick->id)) pick = &npcs[i];
            }
            char msg[200];
            if (pick) {
                if (!a_isHost) Pulsar::RemotePlayer::NoteLocalAttackForTest();
                const float fraction = a_isHost ? 0.4f : 0.3f;
                s_state.dmgTarget = pick->id;
                s_state.dmgBefore = SafeHitBySelf(pick->actor, fraction);
                s_state.dmgAfter = SafeHealth(pick->actor);
                s_state.dmgHitWallMs = WallMs();
                std::snprintf(msg, sizeof(msg), "damage: hit %08X (%.1f m) by %.0f%%, health %.1f -> %.1f (%d synced alive within %.0f m)",
                              pick->id, pick->dist, fraction * 100.0f, s_state.dmgBefore, s_state.dmgAfter, alive, s_state.cfg.lootRadius);
            } else {
                std::snprintf(msg, sizeof(msg), "damage: no health-synced living NPC within %.0f m (%d NPCs)", s_state.cfg.lootRadius, n);
            }
            s_state.actionInfo = msg;
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            Trace(msg);
            PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
        }

        // Takeoff (host): sit in the pilot seat, then, once seated, ask for the
        // launch the way the takeoff menu does (TakeoffMenu_Launch event).
        void TickTakeoff(RE::PlayerCharacter* a_player, const ActorSnap& a_me, std::uint64_t a_now);

        void FinishLoot(const char* a_info)
        {
            s_state.actionInfo = a_info;
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            s_state.lootPhase = LootPhase::kDone;
            Trace(a_info);
            PLOG_INFO("[PULSAR] TestBot: loot action: {}"sv, s_state.actionInfo);
        }

        void TickLoot(RE::PlayerCharacter* a_player, const ActorSnap& a_me, std::uint64_t a_now)
        {
            constexpr int kMaxTries = 10;   // essential NPCs (companions) never die
            static std::uint32_t s_tried[kMaxTries] = {};
            char msg[200];

            if (s_state.lootPhase == LootPhase::kIdle) {
                NpcSeen npcs[64];
                const int n = SafeListNpcs(a_player->parentCell, a_me.x, a_me.y, a_me.z,
                                           s_state.cfg.lootRadius, npcs, 64);
                const NpcSeen* pick = nullptr;
                int alive = 0, guarded = 0;
                for (int i = 0; i < n; ++i) {
                    if (npcs[i].dead) continue;
                    ++alive;
                    if (npcs[i].guarded) { ++guarded; continue; }
                    bool tried = false;
                    for (int t = 0; t < s_state.lootTries; ++t) tried = tried || s_tried[t] == npcs[i].id;
                    if (tried || Pulsar::WorldSync::Loot::CountInventoryItems(npcs[i].actor) <= 0) continue;
                    if (!pick || npcs[i].dist < pick->dist) pick = &npcs[i];
                }
                if (!pick || s_state.lootTries >= kMaxTries) {
                    std::snprintf(msg, sizeof(msg), "loot: no NPC to kill within %.0f m (npcs=%d alive=%d essential/protected=%d tries=%d)",
                                  s_state.cfg.lootRadius, n, alive, guarded, s_state.lootTries);
                    FinishLoot(msg);
                    return;
                }
                s_tried[s_state.lootTries++] = pick->id;
                std::snprintf(msg, sizeof(msg), "loot: killing %08X (%.1f m)", pick->id, pick->dist);
                Trace(msg);
                // Killer = the victim itself. A kill by the player is a crime in
                // a city: the guards downed the host in run 20261028-091213 and
                // the client reloaded mid-test. A null killer took the host down
                // (run 20261028-092756).
                const bool fired = Pulsar::NPCSync::KillActor(pick->actor, pick->actor);
                std::snprintf(msg, sizeof(msg), "loot: kill %08X fired=%d", pick->id, fired ? 1 : 0);
                Trace(msg);
                s_state.lootTarget = pick->id;
                s_state.lootKilledAtMs = a_now;
                s_state.lootDeadAtMs = 0;
                s_state.lootPhase = LootPhase::kKilled;
                return;
            }

            if (s_state.lootPhase != LootPhase::kKilled) return;
            auto* form = RE::TESForm::LookupByID(s_state.lootTarget);
            auto* corpse = form ? form->As<RE::Actor>() : nullptr;
            if (!corpse || !SafeIsDead(corpse)) {
                // The engine refuses to kill essential NPCs; try the next one.
                if (s_state.lootDeadAtMs != 0 || a_now - s_state.lootKilledAtMs > 6000) {
                    // Seen dead, then alive or gone: run 20261028-011644's spawner
                    // NPC 0037C634 (dynamic base) died and was back within 7 s.
                    std::snprintf(msg, sizeof(msg), s_state.lootDeadAtMs ? "loot: %08X was dead, now alive or gone, next"
                                                                         : "loot: %08X did not die, next",
                                  s_state.lootTarget);
                    Trace(msg);
                    s_state.lootPhase = LootPhase::kIdle;
                }
                return;
            }
            if (s_state.lootDeadAtMs == 0) {
                s_state.lootDeadAtMs = a_now;
                std::snprintf(msg, sizeof(msg), "loot: %08X dead, items=%d", s_state.lootTarget,
                              Pulsar::WorldSync::Loot::CountInventoryItems(corpse));
                Trace(msg);
            }
            if (a_now - s_state.lootDeadAtMs < s_state.cfg.lootAfterDeathMs) return;

            const int before = Pulsar::WorldSync::Loot::CountInventoryItems(corpse);
            Trace("loot: taking all");
            const int moved = Pulsar::WorldSync::Loot::TakeAllForTest(corpse, a_player);
            const int after = Pulsar::WorldSync::Loot::CountInventoryItems(corpse);
            std::snprintf(msg, sizeof(msg), "loot: corpse %08X killed, items before=%d after=%d moved=%d",
                          s_state.lootTarget, before, after, moved);
            FinishLoot(msg);
        }

        bool SafeMovePlayer(RE::PlayerCharacter* a_player, float a_x, float a_y, float a_z) noexcept
        {
            __try {
                RE::NiPoint3A target(a_x, a_y, a_z);
                a_player->SetPosition(target, true);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Append one JSON line per sample to a log opened for shared reading. The
        // first version replaced the file each second (MoveFileEx); on the first
        // two-copy run the launcher's once-a-second read fell into step with the
        // host's write and every replace failed for six minutes, so the launcher
        // saw a frozen host. An append-only log cannot collide and loses nothing.
        void WriteState(const std::string& a_json)
        {
            static std::FILE* s_file = nullptr;
            if (!s_file) {
                s_file = _fsopen(s_state.cfg.stateFile.c_str(), "a", _SH_DENYWR);
                if (!s_file) return;
            }
            std::fputs(a_json.c_str(), s_file);
            std::fflush(s_file);
        }

        std::string Num(float a_v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", a_v);
            return buf;
        }

        void TickTakeoff(RE::PlayerCharacter* a_player, const ActorSnap& a_me, std::uint64_t a_now)
        {
            char msg[200];
            if (s_state.takeoffStep == 0) {
                LogFurnitureInCell(a_player->parentCell, a_me.x, a_me.y, a_me.z, 40.0f);
                auto* seat = FindPilotSeat(a_player->parentCell, a_me.x, a_me.y, a_me.z);
                if (!seat) {
                    const ShipSnap sh = SnapShip(a_player);
                    std::snprintf(msg, sizeof(msg), "takeoff: no pilot seat in cell %08X (ship=%08X inSpace=%d)",
                                  a_me.cellID, sh.ship, sh.inSpace ? 1 : 0);
                    s_state.actionInfo = msg;
                    s_state.acted = true;
                    s_state.actedAtWallMs = WallMs();
                    Trace(msg);
                    return;
                }
                std::snprintf(msg, sizeof(msg), "takeoff: sitting in pilot seat %08X", seat->formID);
                Trace(msg);
                const int r = SafeActivateAsPlayer(seat, a_player);
                std::snprintf(msg, sizeof(msg), "seat %08X activate=%d", seat->formID, r);
                s_state.takeoffSeatInfo = msg;
                Trace(msg);
                PLOG_INFO("[PULSAR] TestBot: takeoff {}"sv, s_state.takeoffSeatInfo);
                s_state.takeoffStep = 1;
                s_state.takeoffSatAtMs = a_now;
                return;
            }
            if (s_state.takeoffStep == 1) {
                const ShipSnap sh = SnapShip(a_player);
                // Seated, or 15 s without it: go on either way and record it.
                if (!sh.seated && a_now - s_state.takeoffSatAtMs < 15000) return;
                if (a_now - s_state.takeoffSatAtMs < 3000) return;
                Trace("takeoff: opening the takeoff menu");
                Pulsar::SpaceSync::OpenTakeoffMenuForTest();
                s_state.takeoffStep = 2;
                s_state.takeoffSatAtMs = a_now;
                return;
            }
            if (s_state.takeoffStep == 2) {
                if (a_now - s_state.takeoffSatAtMs < 3000) return;   // let the menu settle
                const ShipSnap sh = SnapShip(a_player);
                Trace("takeoff: asking for the launch");
                const bool menu = Pulsar::SpaceSync::IsTakeoffMenuOpenForTest();
                const bool fired = s_state.cfg.takeoffMenuOnly ? true : Pulsar::SpaceSync::FireTakeoffLaunchForTest();
                if (s_state.cfg.takeoffMenuOnly) Trace("takeoff: TakeoffMenuOnly - launch event NOT fired");
                std::snprintf(msg, sizeof(msg), "takeoff: %s, seated=%d, launch fired=%d, menu=%d",
                              s_state.takeoffSeatInfo.c_str(), sh.seated ? 1 : 0, fired ? 1 : 0, menu ? 1 : 0);
                s_state.actionInfo = msg;
                s_state.takeoffStep = 3;
                s_state.takeoffSatAtMs = a_now;
                s_state.acted = true;
                s_state.actedAtWallMs = WallMs();
                Trace(msg);
                PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
                return;
            }
            // TakeoffF8DuringVote: 3 s into the vote (the client answers after 10 s), the
            // pilot presses F8, the regroup key, through the real key handler.
            if (s_state.takeoffStep == 3 && s_state.cfg.takeoffF8DuringVote && !s_state.f8AtWallMs &&
                a_now - s_state.takeoffSatAtMs >= 3000) {
                Pulsar::SpaceSync::PressF8ForTest();
                s_state.f8AtWallMs = WallMs();
                Trace("takeoff: pressed F8 during the vote");
                PLOG_INFO("[PULSAR] TestBot: takeoff: pressed F8 during the vote"sv);
            }
        }
    }

    namespace
    {
        std::uint32_t ActionID(const char* a_editorID)
        {
            auto* form = RE::TESForm::LookupByEditorID(a_editorID);
            return form ? form->formID : 0;
        }

        // Every frame: the jetpack steps (they are sub-second) and the height tracking.
        void TickJetpack(std::uint64_t a_now)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const ActorSnap me = Snap(player);
            if (me.valid) {
                s_state.jetZMin = (std::min)(s_state.jetZMin, me.z);
                s_state.jetZMax = (std::max)(s_state.jetZMax, me.z);
                const int js = player ? SafeGraphInt(player, "iSyncJumpState") : -1;
                if (js >= 0 && js < 8) s_state.jetJumpMask |= 1 << js;
            }
            const ActorSnap cl = Snap(Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor());
            if (cl.valid && cl.has3D) {
                s_state.jetCloneZMin = (std::min)(s_state.jetCloneZMin, cl.z);
                s_state.jetCloneZMax = (std::max)(s_state.jetCloneZMax, cl.z);
            }
            if (s_state.jetStep == 0 || s_state.jetStep >= 3 || !player) return;
            char msg[120];
            if (s_state.jetStep == 1 && a_now - s_state.jetStepMs >= 250) {
                const std::uint32_t id = ActionID("ActionBoostpackStart");
                const bool ok = id && Pulsar::RemotePlayer::PerformActionOnActor(player, id);
                std::snprintf(msg, sizeof(msg), ", boostpack start %08X ok=%d", id, ok ? 1 : 0);
                s_state.jetInfo += msg;
                s_state.jetStep = 2;
                s_state.jetStepMs = a_now;
                Trace(msg);
            } else if (s_state.jetStep == 2 && a_now - s_state.jetStepMs >= 2500) {
                const std::uint32_t id = ActionID("ActionBoostpackStop");
                const bool ok = id && Pulsar::RemotePlayer::PerformActionOnActor(player, id);
                std::snprintf(msg, sizeof(msg), ", boostpack stop %08X ok=%d", id, ok ? 1 : 0);
                s_state.jetInfo += msg;
                s_state.jetStep = 3;
                Trace(msg);
            }
            s_state.actionInfo = "jetpack: " + s_state.jetInfo;
        }
    }

    // Aim phases from the start: 0-3 s draw, 3-11 s up and aiming, 11-19 s down and
    // aiming, 19-27 s level, not aiming; then the player's own aim again.
    bool AimOverrideForTest(float& a_pitch, bool& a_sighted)
    {
        if (!s_state.cfg.enabled || s_state.cfg.scenario != "aim" || !s_state.aimStartMs) return false;
        const std::uint64_t t = ::GetTickCount64() - s_state.aimStartMs;
        if (t < 3000 || t >= 27000) return false;
        if (t < 11000)      { a_pitch = 0.6f;  a_sighted = true; }
        else if (t < 19000) { a_pitch = -0.6f; a_sighted = true; }
        else                { a_pitch = 0.0f;  a_sighted = false; }
        return true;
    }

    void Tick()
    {
        if (!s_state.loaded) {
            s_state.loaded = true;
            s_state.cfg = LoadSettings();
            if (s_state.cfg.enabled)
                PLOG_INFO("[PULSAR] TestBot: enabled, state file {}"sv, s_state.cfg.stateFile);
        }
        if (!s_state.cfg.enabled) return;

        const std::uint64_t now = ::GetTickCount64();
        if (s_state.cfg.scenario == "jetpack") TickJetpack(now);
        if (now < s_state.nextWriteMs) return;
        s_state.nextWriteMs = now + 1000;

        const bool isHost    = Pulsar::GetConfig().isHost;
        auto* player         = RE::PlayerCharacter::GetSingleton();
        const ActorSnap me   = Snap(player);
        const bool inWorld   = Pulsar::InventorySync::DirectRestore::IsPlayerReadyForConnect();
        const bool connected = Pulsar::Network::GetSingleton().IsConnected();
        auto* clone          = Pulsar::RemotePlayer::GetRemotePlayerManager().GetRemoteActor();
        const ActorSnap cl   = Snap(clone);
        const bool cloneVisible = connected && cl.valid && cl.has3D;

        if (cloneVisible) {
            if (s_state.cloneSeenSinceMs == 0) s_state.cloneSeenSinceMs = now;
            if (s_state.firstCloneSeenMs == 0) s_state.firstCloneSeenMs = now;
        } else {
            s_state.cloneSeenSinceMs = 0;
        }

        // Reward: both roles pick the same weapon from the shared save.
        if (s_state.cfg.scenario == "reward" && !s_state.rewardItem && me.valid && inWorld)
            s_state.rewardItem = Pulsar::WorldSync::Loot::LowestItemOfTypeForTest(
                player, static_cast<std::uint8_t>(RE::FormType::kWEAP));

        // Reward, client: the host's reward arrived; the client's own copy of the
        // quest gives the weapon too, inside the hold, as a replicated stage
        // script would. The mirror must not make it two.
        if (!isHost && s_state.cfg.scenario == "reward" && !s_state.rewardOwnCopy && s_state.rewardItem &&
            Pulsar::QuestSync::Reward::PeerRewardsReceived() > 0) {
            s_state.rewardOwnCopy = true;
            const bool ok = GiveAsQuestReward(player, s_state.rewardItem, 1);
            char msg[120];
            std::snprintf(msg, sizeof(msg), "reward: own quest copy gave item %08X x1 given=%d",
                          s_state.rewardItem, ok ? 1 : 0);
            s_state.actionInfo = msg;
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
        }

        // Loot: kill, wait for the death to reach the client, then take all.
        if (isHost && s_state.cfg.scenario == "loot" && !s_state.acted && me.valid &&
            (s_state.lootPhase != LootPhase::kIdle ||
             (cloneVisible && now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs))) {
            TickLoot(player, me, now);
        }

        // Message check: the test message at +0, +6 and +12 s after the act.
        if (s_state.acted && !s_state.cfg.notifyTest.empty() && s_state.notifySent < 3) {
            const std::uint64_t since = WallMs() - s_state.actedAtWallMs;
            if (since >= static_cast<std::uint64_t>(s_state.notifySent) * 6000) {
                ++s_state.notifySent;
                const std::string text = s_state.cfg.notifyTest + " " + std::to_string(s_state.notifySent);
                Pulsar::UI::HudMessage::Show(text.c_str());
            }
        }

        // Takeoff, client: answer the pilot's request 10 s after it arrives, so
        // the test can tell a held launch from one that did not wait.
        if (!isHost && TakeoffLike() && !s_state.voteReadyWallMs) {
            if (!s_state.voteSeenAtMs && Pulsar::Coop::TakeoffVote::PendingRequestForTest()) {
                s_state.voteSeenAtMs = now;
                s_state.voteSeenWallMs = WallMs();
                Trace("takeoff: partner's request arrived");
            }
            if (s_state.voteSeenAtMs && now - s_state.voteSeenAtMs >= 10000) {
                Pulsar::Coop::TakeoffVote::AnswerReadyForTest();
                s_state.voteReadyWallMs = WallMs();
                s_state.acted = true;
                s_state.actedAtWallMs = s_state.voteReadyWallMs;
                s_state.actionInfo = "takeoff: answered ready";
                Trace("takeoff: answered ready");
            }
        }

        // Takeoff, client (TakeoffPartnerOutside): leave the landed ship through
        // its hatch 20 s after the partner shows up, so the vote's "go" has to
        // move this player back aboard.
        if (!isHost && TakeoffLike() && s_state.cfg.takeoffPartnerOutside &&
            !s_state.partnerExited && cloneVisible && me.valid && now - s_state.cloneSeenSinceMs >= 20000) {
            s_state.partnerExited = true;
            DoorScan scan;
            constexpr int kMaxDoors = 8;
            RE::TESObjectREFR* doors[kMaxDoors] = {};
            float dist[kMaxDoors] = {};
            const int found = SafeFindLoadDoors(player->parentCell, me.x, me.y, me.z, s_state.cfg.doorRadius,
                                                scan, doors, dist, kMaxDoors);
            char msg[160];
            int result = 0, tried = 0;
            for (; tried < found && result != 1; ++tried)
                result = SafeActivateAsPlayer(doors[tried], player);
            std::snprintf(msg, sizeof(msg), "left the ship: %d load doors, tried %d, activate=%d, from cell %08X",
                          found, tried, result, me.cellID);
            s_state.partnerExitInfo = msg;
            Trace(msg);
            PLOG_INFO("[PULSAR] TestBot: takeoff {}"sv, s_state.partnerExitInfo);
        }

        // Takeoff: sit down, then ask for the launch. With the partner outside,
        // its clone leaves view, so time from the first sighting instead.
        const bool takeoffDue = s_state.cfg.takeoffPartnerOutside
            ? (connected && s_state.firstCloneSeenMs && now - s_state.firstCloneSeenMs >= s_state.cfg.moveAfterMs)
            : (cloneVisible && now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs);
        const bool f8Pending = s_state.takeoffStep == 3 && s_state.cfg.takeoffF8DuringVote && !s_state.f8AtWallMs;
        if (isHost && TakeoffLike() && (!s_state.acted || f8Pending) && me.valid &&
            (s_state.takeoffStep != 0 || takeoffDue)) {
            TickTakeoff(player, me, now);
        }

        // Friendly fire, host: two hits on the partner's figure, 4 s apart.
        if (isHost && s_state.cfg.scenario == "friendlyfire" && s_state.ffHits < 2 && cloneVisible &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs &&
            (s_state.ffHits == 0 || now - s_state.ffLastHitMs >= 4000)) {
            const float fraction = s_state.ffHits == 0 ? 0.6f : 1.5f;
            const float before = SafeHitAsPlayer(clone, player, fraction);
            const float after = SafeHealth(clone);
            ++s_state.ffHits;
            s_state.ffLastHitMs = now;
            char msg[160];
            std::snprintf(msg, sizeof(msg), "%shit %d: %.0f%% of %.0f, figure health right after %.0f",
                          s_state.ffHitInfo.empty() ? "" : "; ", s_state.ffHits, fraction * 100.0f, before, after);
            s_state.ffHitInfo += msg;
            Trace(msg);
            if (s_state.ffHits == 1) {
                s_state.acted = true;
                s_state.actedAtWallMs = WallMs();
            }
            s_state.actionInfo = "friendlyfire: " + s_state.ffHitInfo;
            PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
        }

        // Reconnect, client: a 25 s network outage once the partner is settled.
        if (!isHost && s_state.cfg.scenario == "reconnect" && !s_state.acted && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            Pulsar::Network::GetSingleton().SimulateOutageForTest(25000);
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            s_state.actionInfo = "reconnect: network outage 25000 ms";
            Trace(s_state.actionInfo.c_str());
            PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
        }

        // Pickup, host: take the nearest loose item, as the player would (Activate core).
        if (isHost && s_state.cfg.scenario == "pickup" && !s_state.acted && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            ItemSeen items[64];
            const int n = SafeListLooseItems(player->parentCell, me.x, me.y, me.z, 20.0f, items, 64);
            const ItemSeen* pick = nullptr;
            for (int i = 0; i < n; ++i)
                if (items[i].present && (!pick || items[i].dist < pick->dist)) pick = &items[i];
            char msg[160];
            if (pick) {
                const int r = SafeActivateAsPlayer(pick->ref, player);
                s_state.pickRef = pick->id;
                std::snprintf(msg, sizeof(msg), "pickup: took %08X (%.1f m), activate=%d, %d loose items within 20 m",
                              pick->id, pick->dist, r, n);
            } else {
                std::snprintf(msg, sizeof(msg), "pickup: no loose item within 20 m (%d seen, none present)", n);
            }
            s_state.actionInfo = msg;
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            Trace(msg);
            PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
        }

        // Jetpack, host: jump; TickJetpack fires the boostpack 250 ms later.
        if (isHost && s_state.cfg.scenario == "jetpack" && s_state.jetStep == 0 && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            const std::uint32_t id = ActionID("ActionJump");
            const bool ok = id && Pulsar::RemotePlayer::PerformActionOnActor(player, id);
            char msg[80];
            std::snprintf(msg, sizeof(msg), "jump %08X ok=%d from z %.2f", id, ok ? 1 : 0, me.z);
            s_state.jetInfo = msg;
            s_state.jetStep = 1;
            s_state.jetStepMs = now;
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            s_state.actionInfo = "jetpack: " + s_state.jetInfo;
            Trace(msg);
            PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
        }

        // Separate characters: both players change their own character once settled;
        // the host then bridges (a fresh save loaded on the client) 30 s later, after
        // the client's 5 s live capture has seen its change.
        if (s_state.cfg.scenario == "separate" && !s_state.acted && cloneVisible && me.valid && inWorld &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs)
            ActSeparate(player, isHost);
        if (isHost && s_state.cfg.scenario == "separate" && s_state.acted && !s_state.sepBridgeWallMs &&
            s_state.sepBridgeTries < 60 && now - s_state.sepActMs >= 30000) {
            ++s_state.sepBridgeTries;
            if (Pulsar::SpaceSync::FastTravel::FireSaveBridge("bot-separate", /*a_triggerOwnSave=*/true)) {
                s_state.sepBridgeWallMs = WallMs();
                Trace("separate: save bridge fired");
                PLOG_INFO("[PULSAR] TestBot: separate: save bridge fired (try {})"sv, s_state.sepBridgeTries);
            }
        }

        // Damage: the host hits once settled, the client 20 s later.
        if (s_state.cfg.scenario == "damage" && !s_state.acted && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs + (isHost ? 0 : 20000))
            ActDamage(player, me, isHost);

        // Aim, host: draw the weapon, then the phases run from AimOverrideForTest.
        if (isHost && s_state.cfg.scenario == "aim" && !s_state.aimStartMs && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            s_state.aimStartMs = now;
            s_state.aimDrawn = SafeDrawWeapon(player);
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            s_state.actionInfo = std::string("aim: weapon draw called=") + (s_state.aimDrawn ? "1" : "0") +
                                 ", up +0.6 rad 3-11 s, down -0.6 rad 11-19 s, level 19-27 s";
            Trace(s_state.actionInfo.c_str());
            PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
        }

        // Scene (host): start scenes of running Akila quests, one every 4 s, until
        // the host's game plays one. Background quests can only be started by the
        // story manager (run 20261030-225533), so the scene's quest must already run.
        if (isHost && s_state.cfg.scenario == "scene" && !s_state.acted && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            if (!s_state.sceneSearched) {
                s_state.sceneSearched = true;
                if (s_state.cfg.sceneId) {
                    s_state.sceneCands = { s_state.cfg.sceneId };
                    s_state.sceneSearchInfo = "SceneID from the ini";
                } else {
                    s_state.sceneCands = Pulsar::SceneSync::RunningScenesForTest("akila", 40, s_state.sceneSearchInfo);
                }
                Trace(("scene: " + std::to_string(s_state.sceneCands.size()) + " candidates, " + s_state.sceneSearchInfo).c_str());
            }
            const bool tried = s_state.sceneTryMs != 0;
            const std::uint32_t cur = s_state.sceneIdx < s_state.sceneCands.size() ? s_state.sceneCands[s_state.sceneIdx] : 0;
            char msg[260];
            if (tried && cur && Pulsar::SceneSync::LastStartWallMsForTest(cur) + 500 >= s_state.sceneTryWallMs) {
                std::snprintf(msg, sizeof(msg), "scene: %08X found=1 started on try %zu of %zu (%s)", cur,
                              s_state.sceneIdx + 1, s_state.sceneCands.size(), s_state.sceneSearchInfo.c_str());
                s_state.actionInfo = msg;
                s_state.acted = true;
                s_state.actedAtWallMs = s_state.sceneTryWallMs;
                Trace(msg);
                PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
            } else if (!tried || now - s_state.sceneTryMs >= 4000) {
                if (tried) ++s_state.sceneIdx;
                if (s_state.sceneIdx >= s_state.sceneCands.size()) {
                    std::snprintf(msg, sizeof(msg), "scene: none of %zu candidates started (%s)",
                                  s_state.sceneCands.size(), s_state.sceneSearchInfo.c_str());
                    s_state.actionInfo = msg;
                    s_state.acted = true;
                    s_state.actedAtWallMs = WallMs();
                    Trace(msg);
                    PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
                } else {
                    const std::uint32_t next = s_state.sceneCands[s_state.sceneIdx];
                    s_state.sceneTryMs = now;
                    s_state.sceneTryWallMs = WallMs();
                    Pulsar::SceneSync::StartSceneForTest(next);
                    std::snprintf(msg, sizeof(msg), "scene: trying %08X (%zu of %zu)", next, s_state.sceneIdx + 1,
                                  s_state.sceneCands.size());
                    Trace(msg);
                }
            }
        }

        // Bridges: the host sends 6 save bridges, one every 45 s, while the game runs;
        // the client must load each. A save sent from the network thread without the
        // ENet lock went missing about 1 in 5 (runs 20261030-212343, 20261031-014412).
        if (s_state.cfg.scenario == "bridges" && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            if (isHost && s_state.bridgesFired < 6) {
                if (!s_state.bridgeNextMs) s_state.bridgeNextMs = now;
                if (now >= s_state.bridgeNextMs &&
                    Pulsar::SpaceSync::FastTravel::FireSaveBridge("bot-bridges", /*a_triggerOwnSave=*/true)) {
                    ++s_state.bridgesFired;
                    s_state.bridgesFiredAt += (s_state.bridgesFiredAt.empty() ? "" : ",") + std::to_string(WallMs());
                    s_state.bridgeNextMs = now + 45000;
                    s_state.acted = true;
                    if (!s_state.actedAtWallMs) s_state.actedAtWallMs = WallMs();
                    PLOG_INFO("[PULSAR] TestBot: bridges: fired {} of 6"sv, s_state.bridgesFired);
                }
            }
        }
        if (s_state.cfg.scenario == "bridges" && !isHost) {
            const std::uint64_t load = Pulsar::WorldSync::GetLastLoadCompletedTick();
            if (load != 0 && load != s_state.bridgeLastLoad) {
                if (s_state.bridgeLastLoad != 0) {   // the first one is the join load
                    ++s_state.bridgeLoads;
                    s_state.bridgeLoadsAt += (s_state.bridgeLoadsAt.empty() ? "" : ",") + std::to_string(WallMs());
                }
                s_state.bridgeLastLoad = load;
            }
        }

        // Space (host): 30 s after reaching space, move the ship 2 km off The Eye,
        // the station over Jemison (the takeoff save leaves from New Atlantis; the
        // orbit right after takeoff is empty, run 20261030-215716), then bridge the
        // save as an arrival does. Game.FastTravel moved the player out of the ship
        // instead (run 20261031-003619) and star-map travel has no script call, so
        // this skips the jump itself: it tests what both players see after a
        // bridge at a station.
        if (isHost && s_state.cfg.scenario == "space" && s_state.acted && !s_state.travelAtWallMs) {
            const ShipSnap sh = SnapShip(player);
            if (sh.inSpace && !s_state.spaceSinceMs) s_state.spaceSinceMs = now;
            if (s_state.spaceSinceMs && now - s_state.spaceSinceMs >= 30000) {
                const auto eye = RefsByName("theeyeref", 4);
                auto* dest = eye.empty() ? nullptr : RE::TESForm::LookupByID<RE::TESObjectREFR>(eye.front().first);
                auto* ship = player->GetSpaceship(true);
                bool sent = false;
                if (dest && ship) {
                    auto obj = Pulsar::GameCalls::ObjectFor(static_cast<RE::TESObjectREFR*>(ship));
                    sent = obj && Pulsar::GameCalls::CallMethod(Pulsar::GameCalls::VM(), obj, "MoveTo",
                        { Pulsar::GameCalls::Arg(dest), Pulsar::GameCalls::Arg(2000.0f), Pulsar::GameCalls::Arg(0.0f),
                          Pulsar::GameCalls::Arg(0.0f), Pulsar::GameCalls::Arg(true), Pulsar::GameCalls::Arg(false) });
                    // Then turn the ship to face the station, so the screenshots of both
                    // players show it: heading + the angle to the station, as the game
                    // measures it (GetHeadingAngle).
                    if (sent) {
                        auto* vm = Pulsar::GameCalls::VM();
                        Pulsar::GameCalls::CallMethodForResult(vm, obj, "GetAngleZ", {},
                            [vm, obj, dest](const RE::BSScript::Variable& a_z) {
                                const float z = a_z.is<float>() ? RE::BSScript::get<float>(a_z) : 0.0f;
                                Pulsar::GameCalls::CallMethodForResult(vm, obj, "GetHeadingAngle", { Pulsar::GameCalls::Arg(dest) },
                                    [vm, obj, z](const RE::BSScript::Variable& a_h) {
                                        const float h = a_h.is<float>() ? RE::BSScript::get<float>(a_h) : 0.0f;
                                        Pulsar::GameCalls::CallMethod(vm, obj, "SetAngle",
                                            { Pulsar::GameCalls::Arg(0.0f), Pulsar::GameCalls::Arg(0.0f), Pulsar::GameCalls::Arg(z + h) });
                                        PLOG_INFO("[PULSAR] TestBot: space: turned the ship to face the station ({:.0f} + {:.0f} deg)"sv, z, h);
                                    });
                            });
                    }
                }
                char msg[200];
                std::snprintf(msg, sizeof(msg), "travel: ship %08X moved to %08X TheEyeREF queued=%d",
                              ship ? ship->formID : 0u, dest ? dest->formID : 0u, sent ? 1 : 0);
                s_state.travelInfo = msg;
                s_state.travelAtWallMs = WallMs();
                s_state.sepActMs = now;   // bridge timer, shared with "separate"
                Trace(msg);
                PLOG_INFO("[PULSAR] TestBot: space: {}"sv, s_state.travelInfo);
            }
        }
        if (isHost && s_state.cfg.scenario == "space" && s_state.travelAtWallMs && !s_state.sepBridgeWallMs &&
            s_state.sepBridgeTries < 60 && now - s_state.sepActMs >= 15000) {
            ++s_state.sepBridgeTries;
            if (Pulsar::SpaceSync::FastTravel::FireSaveBridge("bot-space", /*a_triggerOwnSave=*/true)) {
                s_state.sepBridgeWallMs = WallMs();
                s_state.travelInfo += "; bridge fired";
                PLOG_INFO("[PULSAR] TestBot: space: save bridge fired (try {})"sv, s_state.sepBridgeTries);
            }
        }

        // Mission: both sides pick the same running mission-board quest (story
        // manager, "MB_"); the client stops it quietly (the host has a mission the
        // client lacks), and 30 s later the host advances a stage in it, as
        // accepting a mission does. The client must end with the quest running at
        // the host's stage (its targets come with the host's save).
        if (s_state.cfg.scenario == "mission" && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            if (!s_state.missionPicked) {
                s_state.missionPicked = true;
                s_state.missionPickedMs = now;
                s_state.missionLoadAtPick = Pulsar::WorldSync::GetLastLoadCompletedTick();
                const auto mb = Pulsar::QuestSync::RunningStoryQuestsForTest("mb_", 20);
                const auto all = Pulsar::QuestSync::RunningStoryQuestsForTest("", 30);
                std::string names;
                for (const auto id : all) {
                    auto* f = RE::TESForm::LookupByID(id);
                    const char* eid = f ? f->GetFormEditorID() : nullptr;
                    names += (names.empty() ? "" : " ") + std::string(eid ? eid : "?");
                }
                PLOG_INFO("[PULSAR] TestBot: mission: {} running MB_ quests; running story quests: {}"sv, mb.size(), names);
                s_state.missionId = mb.empty() ? 0 : mb.front();
                char msg[200];
                auto* f = s_state.missionId ? RE::TESForm::LookupByID(s_state.missionId) : nullptr;
                std::snprintf(msg, sizeof(msg), "mission: picked %08X %s (%zu running MB_ quests)", s_state.missionId,
                              f && f->GetFormEditorID() ? f->GetFormEditorID() : "-", mb.size());
                Trace(msg);
                if (!s_state.missionId) {
                    s_state.actionInfo = msg;
                    s_state.acted = true;
                    s_state.actedAtWallMs = WallMs();
                }
            }
            if (s_state.missionId && !isHost && !s_state.missionStopped) {
                s_state.missionStopped = true;
                s_state.missionStoppedWallMs = WallMs();
                const bool q = Pulsar::QuestSync::StopQuietlyForTest(s_state.missionId);
                Trace(q ? "mission: stopped it here quietly" : "mission: quiet stop not queued");
            }
            if (s_state.missionId && isHost && !s_state.acted && now - s_state.missionPickedMs >= 30000) {
                const bool q = Pulsar::QuestSync::AdvanceStageForTest(s_state.missionId);
                char msg[200];
                auto* f = RE::TESForm::LookupByID(s_state.missionId);
                std::snprintf(msg, sizeof(msg), "mission: %08X found=1 %s advance queued=%d", s_state.missionId,
                              f && f->GetFormEditorID() ? f->GetFormEditorID() : "-", q ? 1 : 0);
                s_state.actionInfo = msg;
                s_state.acted = true;
                s_state.actedAtWallMs = WallMs();
                Trace(msg);
                PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
            }
            if (s_state.missionId && now - s_state.missionRefreshMs >= 2000) {
                s_state.missionRefreshMs = now;
                Pulsar::QuestSync::RefreshStageForTest(s_state.missionId);
            }
        }

        // Quest (host): start stopped Akila quests (not main quest), one every 4 s,
        // until one runs here; the client must start the same quest. Shares the
        // scene loop's candidate fields.
        if (isHost && s_state.cfg.scenario == "quest" && !s_state.acted && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            if (!s_state.sceneSearched) {
                s_state.sceneSearched = true;
                std::size_t seen = 0;
                for (const auto id : Pulsar::QuestSync::StoppedQuestsForTest("akila", 200, seen)) {
                    auto* f = RE::TESForm::LookupByID(id);
                    const char* eid = f ? f->GetFormEditorID() : nullptr;
                    if (eid && (eid[0] == 'M' || eid[0] == 'm') && (eid[1] == 'Q' || eid[1] == 'q')) continue;
                    s_state.sceneCands.push_back(id);
                    if (s_state.sceneCands.size() >= 40) break;
                }
                s_state.sceneSearchInfo = std::to_string(s_state.sceneCands.size()) + " stopped Akila quests of " +
                                          std::to_string(seen) + " seen";
                Trace(("quest: " + s_state.sceneSearchInfo).c_str());
            }
            const bool tried = s_state.sceneTryMs != 0;
            const std::uint32_t cur = s_state.sceneIdx < s_state.sceneCands.size() ? s_state.sceneCands[s_state.sceneIdx] : 0;
            char msg[260];
            if (tried && cur && Pulsar::QuestSync::IsRunningForTest(cur)) {
                auto* f = RE::TESForm::LookupByID(cur);
                const char* eid = f ? f->GetFormEditorID() : nullptr;
                std::snprintf(msg, sizeof(msg), "quest: %08X found=1 %s started on try %zu (%s)", cur, eid ? eid : "-",
                              s_state.sceneIdx + 1, s_state.sceneSearchInfo.c_str());
                s_state.actionInfo = msg;
                s_state.acted = true;
                s_state.actedAtWallMs = s_state.sceneTryWallMs;
                Trace(msg);
                PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
            } else if (!tried || now - s_state.sceneTryMs >= 4000) {
                if (tried) ++s_state.sceneIdx;
                if (s_state.sceneIdx >= s_state.sceneCands.size()) {
                    std::snprintf(msg, sizeof(msg), "quest: none of %zu candidates started (%s)",
                                  s_state.sceneCands.size(), s_state.sceneSearchInfo.c_str());
                    s_state.actionInfo = msg;
                    s_state.acted = true;
                    s_state.actedAtWallMs = WallMs();
                    Trace(msg);
                    PLOG_INFO("[PULSAR] TestBot: {}"sv, s_state.actionInfo);
                } else {
                    const std::uint32_t next = s_state.sceneCands[s_state.sceneIdx];
                    s_state.sceneTryMs = now;
                    s_state.sceneTryWallMs = WallMs();
                    const bool q = Pulsar::QuestSync::StartForTest(next);
                    std::snprintf(msg, sizeof(msg), "quest: trying %08X (%zu of %zu) queued=%d", next, s_state.sceneIdx + 1,
                                  s_state.sceneCands.size(), q ? 1 : 0);
                    Trace(msg);
                }
            }
        }

        // The test action, once the partner is settled: host only.
        if (isHost && s_state.cfg.scenario != "loot" && !TakeoffLike() &&
            s_state.cfg.scenario != "friendlyfire" && s_state.cfg.scenario != "aim" &&
            s_state.cfg.scenario != "jetpack" && s_state.cfg.scenario != "pickup" &&
            s_state.cfg.scenario != "separate" && s_state.cfg.scenario != "damage" &&
            s_state.cfg.scenario != "scene" && s_state.cfg.scenario != "quest" && s_state.cfg.scenario != "mission" &&
            s_state.cfg.scenario != "bridges" &&
            !s_state.acted && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            char msg[200];
            if (s_state.cfg.scenario == "door") {
                DoorScan scan;
                Trace("door: scanning the cell");
                constexpr int kMaxDoors = 8;
                RE::TESObjectREFR* doors[kMaxDoors] = {};
                float dist[kMaxDoors] = {};
                const int found = SafeFindLoadDoors(player->parentCell, me.x, me.y, me.z,
                                                    s_state.cfg.doorRadius, scan, doors, dist, kMaxDoors);
                if (found > 0) {
                    int result = 0, tried = 0;
                    for (; tried < found && result != 1; ++tried) {
                        std::snprintf(msg, sizeof(msg), "door: activating %08X (%.1f m)", doors[tried]->formID, dist[tried]);
                        Trace(msg);
                        result = SafeActivateAsPlayer(doors[tried], player);
                        std::snprintf(msg, sizeof(msg), "door: %08X activate=%d", doors[tried]->formID, result);
                        Trace(msg);
                        PLOG_INFO("[PULSAR] TestBot: {}"sv, std::string(msg));
                    }
                    std::snprintf(msg, sizeof(msg), "door %08X of %d load doors (%.1f m away), activate=%d, tried %d",
                                  doors[tried - 1]->formID, scan.loadDoors, dist[tried - 1], result, tried);
                    Trace(msg);
                } else {
                    LogDoorsInCell(player->parentCell, me.x, me.y, me.z);
                    std::snprintf(msg, sizeof(msg),
                                  "no load door found: cell %08X refs=%u doors=%d loadDoors=%d nearestLoadDoor=%.1f m radius=%.0f seh=%d",
                                  me.cellID, scan.refs, scan.doors, scan.loadDoors, scan.nearestLoadDoor,
                                  s_state.cfg.doorRadius, scan.seh ? 1 : 0);
                }
                s_state.actionInfo = msg;
            } else if (s_state.cfg.scenario == "papyrus") {
                // C++ -> Papyrus dispatch check: PULSAR's own static function the
                // move-aboard and follow paths use, aimed at the player itself.
                {
                    // The scrap heap the patched BSScrapArrayAllocator uses: its
                    // vtable slot 0x0B should be Deallocate, RVA 0x22CB6B0.
                    auto* heap = RE::MemoryManager::GetSingleton()->GetThreadScrapHeap();
                    const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
                    const auto* vt = heap ? *reinterpret_cast<std::uintptr_t* const*>(heap) : nullptr;
                    std::snprintf(msg, sizeof(msg), "papyrus: scrap heap %p vtable[0x0A] RVA 0x%llX [0x0B] RVA 0x%llX",
                                  static_cast<void*>(heap),
                                  vt ? static_cast<unsigned long long>(vt[0x0A] - base) : 0ull,
                                  vt ? static_cast<unsigned long long>(vt[0x0B] - base) : 0ull);
                    Trace(msg);
                    PLOG_INFO("[PULSAR] TestBot: {}"sv, std::string(msg));
                }
                Trace("papyrus: DispatchStaticCall PulsarFurnSync.DoPlayerFollowMoveTo(player)");
                const bool ok = Pulsar::SpaceSync::DispatchMoveToForTest(player);
                std::snprintf(msg, sizeof(msg), "papyrus: DoPlayerFollowMoveTo(player) dispatched=%d thread=%lu",
                              ok ? 1 : 0, GetCurrentThreadId());
                Trace(msg);
                s_state.actionInfo = msg;
            } else if (s_state.cfg.scenario == "reward") {
                const bool credits = GiveAsQuestReward(player, kCredits, s_state.cfg.rewardCredits);
                const bool item = s_state.rewardItem && GiveAsQuestReward(player, s_state.rewardItem, 1);
                std::snprintf(msg, sizeof(msg), "reward: credits x%d given=%d, item %08X x1 given=%d",
                              s_state.cfg.rewardCredits, credits ? 1 : 0, s_state.rewardItem, item ? 1 : 0);
                s_state.actionInfo = msg;
            } else {
                const float tx = me.x + s_state.cfg.moveMetres;
                const bool ok = SafeMovePlayer(player, tx, me.y, me.z);
                std::snprintf(msg, sizeof(msg), "moved %.1f m on X -> (%.2f,%.2f,%.2f) ok=%d",
                              s_state.cfg.moveMetres, tx, me.y, me.z, ok ? 1 : 0);
                s_state.actionInfo = msg;
            }
            s_state.acted = true;
            s_state.actedAtWallMs = WallMs();
            PLOG_INFO("[PULSAR] TestBot: {} action: {}"sv, s_state.cfg.scenario, s_state.actionInfo);
        }

        char cellBuf[16], cloneCellBuf[16];
        std::snprintf(cellBuf, sizeof(cellBuf), "%08X", me.cellID);
        std::snprintf(cloneCellBuf, sizeof(cloneCellBuf), "%08X", cl.cellID);
        std::string j = "{";
        j += "\"wallMs\":" + std::to_string(WallMs());
        j += ",\"seq\":" + std::to_string(++s_state.writes);
        j += ",\"pid\":" + std::to_string(GetCurrentProcessId());
        j += std::string(",\"role\":\"") + (isHost ? "host" : "client") + "\"";
        j += std::string(",\"inWorld\":") + (inWorld ? "true" : "false");
        j += std::string(",\"connected\":") + (connected ? "true" : "false");
        j += std::string(",\"playerCell\":\"") + cellBuf + "\"";
        j += ",\"player\":[" + Num(me.x) + "," + Num(me.y) + "," + Num(me.z) + "]";
        j += std::string(",\"cloneVisible\":") + (cloneVisible ? "true" : "false");
        j += std::string(",\"cloneCell\":\"") + cloneCellBuf + "\"";
        j += ",\"clone\":[" + Num(cl.x) + "," + Num(cl.y) + "," + Num(cl.z) + "]";
        // 2-4 players: the figures of players other than the first partner.
        j += ",\"localId\":" + std::to_string(Pulsar::Network::GetSingleton().LocalPlayerId());
        j += ",\"figures\":[";
        {
            bool firstFig = true;
            for (std::uint8_t id = 0; id < 4; ++id) {
                if (id == Pulsar::RemotePlayer::FirstPartner()) continue;
                auto& fig = Pulsar::RemotePlayer::GetRemotePlayerFigure(id);
                if (!fig.IsExtra()) continue;
                const ActorSnap fs = Snap(fig.GetRemoteActor());
                if (!fs.valid) continue;
                j += std::string(firstFig ? "" : ",") + "{\"id\":" + std::to_string(id) + ",\"pos\":[" + Num(fs.x) + "," +
                     Num(fs.y) + "," + Num(fs.z) + "],\"has3D\":" + (fs.has3D ? "true" : "false") + "}";
                firstFig = false;
            }
        }
        j += "]";
        j += ",\"scenario\":\"" + s_state.cfg.scenario + "\"";
        j += std::string(",\"acted\":") + (s_state.acted ? "true" : "false");
        j += ",\"actedAtWallMs\":" + std::to_string(s_state.actedAtWallMs);
        j += ",\"actionInfo\":\"" + s_state.actionInfo + "\"";
        if (s_state.cfg.scenario == "loot") {
            NpcSeen npcs[64];
            const int n = me.valid ? SafeListNpcs(player->parentCell, me.x, me.y, me.z, 60.0f, npcs, 64) : 0;
            // Every save-file NPC nearby, alive or dead: the client's copy of the
            // host's victim is followed even when the death does not reach it.
            j += ",\"npcs\":[";
            for (int i = 0; i < n; ++i) {
                char c[80];
                std::snprintf(c, sizeof(c), "%s{\"id\":\"%08X\",\"dead\":%s,\"items\":%d}", i ? "," : "",
                              npcs[i].id, npcs[i].dead ? "true" : "false",
                              Pulsar::WorldSync::Loot::CountInventoryItems(npcs[i].actor));
                j += c;
            }
            j += "]";
        }
        if (TakeoffLike()) {
            const ShipSnap sh = SnapShip(player);
            char c[120];
            std::snprintf(c, sizeof(c), ",\"ship\":{\"inSpace\":%s,\"ship\":\"%08X\",\"seated\":%s}",
                          sh.inSpace ? "true" : "false", sh.ship, sh.seated ? "true" : "false");
            j += c;
            j += ",\"vote\":{\"state\":\"" + std::string(Pulsar::Coop::TakeoffVote::StateForTest()) + "\"";
            j += ",\"seenAtWallMs\":" + std::to_string(s_state.voteSeenWallMs);
            j += ",\"readyAtWallMs\":" + std::to_string(s_state.voteReadyWallMs);
            j += ",\"f8AtWallMs\":" + std::to_string(s_state.f8AtWallMs) + "}";
            j += ",\"exitInfo\":\"" + s_state.partnerExitInfo + "\"";
            j += ",\"travelInfo\":\"" + s_state.travelInfo + "\"";
        }
        if (s_state.cfg.scenario == "scene") {
            j += ",\"scenes\":\"" + Pulsar::SceneSync::RecentEventsForTest() + "\"";
        }
        if (s_state.cfg.scenario == "quest") {
            j += ",\"questStarts\":\"" + Pulsar::QuestSync::RecentStartsForTest() + "\"";
        }
        if (s_state.cfg.scenario == "bridges") {
            j += ",\"bridges\":{\"fired\":" + std::to_string(s_state.bridgesFired) + ",\"firedAt\":\"" + s_state.bridgesFiredAt +
                 "\",\"loads\":" + std::to_string(s_state.bridgeLoads) + ",\"loadsAt\":\"" + s_state.bridgeLoadsAt +
                 "\",\"savesSent\":" + std::to_string(Pulsar::Network::NetworkHost::SavesSentForTest()) +
                 ",\"savesReceived\":" + std::to_string(Pulsar::WorldSync::SavesReceivedForTest()) + "}";
        }
        if (s_state.cfg.scenario == "mission") {
            char c[220];
            std::snprintf(c, sizeof(c),
                          ",\"mission\":{\"id\":\"%08X\",\"running\":%s,\"stage\":%d,\"stoppedAtWallMs\":%llu,\"loadedSincePick\":%s}",
                          s_state.missionId,
                          s_state.missionId && Pulsar::QuestSync::IsRunningForTest(s_state.missionId) ? "true" : "false",
                          s_state.missionId ? Pulsar::QuestSync::CachedStageForTest(s_state.missionId) : -1,
                          static_cast<unsigned long long>(s_state.missionStoppedWallMs),
                          s_state.missionPicked && Pulsar::WorldSync::GetLastLoadCompletedTick() != s_state.missionLoadAtPick ? "true" : "false");
            j += c;
        }
        if (s_state.cfg.scenario == "space") {
            // Every 10 s while in space: "ID:BASE:TYPE:DIST_KM" for each reference,
            // on that one state line only (a station cell holds thousands); the
            // lines between carry the count alone.
            static std::string s_census = ",\"space\":null";
            static std::string s_summary = ",\"space\":null";
            static std::uint64_t s_censusAt = 0;
            const std::uint64_t t = ::GetTickCount64();
            bool fresh = false;
            if (t - s_censusAt >= 10000) {
                s_censusAt = t;
                fresh = true;
                static SpaceRef refs[4096];
                std::uint32_t cell = 0;
                const int n = SafeSpaceCensus(player, cell, refs, 4096);
                if (!cell) {
                    s_census = ",\"space\":null";
                    s_summary = s_census;
                } else {
                    std::string list;
                    list.reserve(static_cast<std::size_t>(n) * 32);
                    for (int i = 0; i < n; ++i) {
                        char c[48];
                        std::snprintf(c, sizeof(c), "%s%08X:%08X:%02X:%.1f", i ? "," : "",
                                      refs[i].id, refs[i].base, refs[i].type, refs[i].dist / 1000.0f);
                        list += c;
                    }
                    char h[160];
                    std::snprintf(h, sizeof(h), ",\"space\":{\"cell\":\"%08X\",\"atWallMs\":%llu,\"travelAtWallMs\":%llu,\"n\":%d,\"refs\":\"",
                                  cell, static_cast<unsigned long long>(WallMs()),
                                  static_cast<unsigned long long>(s_state.travelAtWallMs), n);
                    s_census = h + list + "\"}";
                    char sm[96];
                    std::snprintf(sm, sizeof(sm), ",\"spaceN\":{\"cell\":\"%08X\",\"n\":%d}", cell, n);
                    s_summary = std::string(",\"space\":null") + sm;
                }
            }
            j += fresh ? s_census : s_summary;
            j += RefCheckJson(player, fresh);
        } else if (!s_state.cfg.checkRefsFile.empty()) {
            // Any other scenario with CheckRefsFile (a session started from a save at
            // a station): the same check on its own 10 s timer.
            static std::uint64_t s_refCheckAt = 0;
            const std::uint64_t t = ::GetTickCount64();
            const bool fresh = t - s_refCheckAt >= 10000;
            if (fresh) s_refCheckAt = t;
            j += RefCheckJson(player, fresh);
            // And how many references the ship's space cell holds, with how many have
            // 3D, so a save made at a station shows whether anything is there.
            static std::string s_cellInfo;
            if (fresh) {
                static SpaceRef refs[4096];
                std::uint32_t cell = 0;
                const int n = SafeSpaceCensus(player, cell, refs, 4096);
                int with3D = 0;
                for (int i = 0; i < n; ++i) {
                    if (SafeRefStatus(refs[i].id).has3D) ++with3D;
                }
                char c[120];
                std::snprintf(c, sizeof(c), ",\"spaceCell\":{\"cell\":\"%08X\",\"n\":%d,\"has3D\":%d}", cell, n, with3D);
                s_cellInfo = cell ? c : "";
            }
            j += s_cellInfo;
        }
        if (s_state.cfg.scenario == "reward") {
            char c[200];
            std::snprintf(c, sizeof(c),
                          ",\"reward\":{\"item\":\"%08X\",\"items\":%d,\"credits\":%d,\"peerRewards\":%u,\"ownCopy\":%s}",
                          s_state.rewardItem,
                          me.valid && s_state.rewardItem ? Pulsar::WorldSync::Loot::CountItemForTest(player, s_state.rewardItem) : -1,
                          me.valid ? Pulsar::WorldSync::Loot::CountItemForTest(player, kCredits) : -1,
                          Pulsar::QuestSync::Reward::PeerRewardsReceived(),
                          s_state.rewardOwnCopy ? "true" : "false");
            j += c;
        }
        if (s_state.cfg.scenario == "reconnect")
            j += ",\"reconnects\":" + std::to_string(Pulsar::Network::GetSingleton().ReconnectsForTest());
        if (s_state.cfg.scenario == "pickup") {
            // Every loose item around this player, with whether it is still there.
            ItemSeen items[48];
            const int n = me.valid ? SafeListLooseItems(player->parentCell, me.x, me.y, me.z, 40.0f, items, 48) : 0;
            j += ",\"items\":[";
            for (int i = 0; i < n; ++i) {
                char c[64];
                std::snprintf(c, sizeof(c), "%s{\"id\":\"%08X\",\"present\":%s}", i ? "," : "", items[i].id,
                              items[i].present ? "true" : "false");
                j += c;
            }
            j += "]";
        }
        if (s_state.cfg.scenario == "jetpack") {
            char c[300];
            std::snprintf(c, sizeof(c),
                          ",\"jet\":{\"step\":%d,\"zMin\":%.3f,\"zMax\":%.3f,\"cloneZMin\":%.3f,\"cloneZMax\":%.3f,"
                          "\"jumpMask\":%d,\"boostStarts\":%u,\"boostAccepted\":%u}",
                          s_state.jetStep, s_state.jetZMin, s_state.jetZMax, s_state.jetCloneZMin, s_state.jetCloneZMax,
                          s_state.jetJumpMask, Pulsar::RemotePlayer::BoostStartsForTest(),
                          Pulsar::RemotePlayer::BoostStartsAcceptedForTest());
            j += c;
            s_state.jetZMin = s_state.jetCloneZMin = 1e9f;
            s_state.jetZMax = s_state.jetCloneZMax = -1e9f;
            s_state.jetJumpMask = 0;
        }
        if (s_state.cfg.scenario == "aim") {
            // Host: what it sends. Client: what arrived, the clone's head and pose.
            float sentPitch = 0.0f;
            bool sentSighted = false;
            const bool steering = AimOverrideForTest(sentPitch, sentSighted);
            float head[9] = {};
            const bool gotHead = !isHost && cloneVisible && SafeHeadRotation(clone, head);
            char c[400];
            std::snprintf(c, sizeof(c),
                          ",\"aim\":{\"steering\":%s,\"sent\":%.3f,\"sentSighted\":%s,\"recv\":%.3f,\"cloneSighted\":%d,"
                          "\"head\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f],\"gotHead\":%s}",
                          steering ? "true" : "false", sentPitch, sentSighted ? "true" : "false",
                          Pulsar::RemotePlayer::GetRemotePlayerManager().GetNetPitch(),
                          cloneVisible ? SafeGraphInt(clone, "iSyncSighted") : -1,
                          head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7], head[8],
                          gotHead ? "true" : "false");
            j += c;
        }
        if (s_state.cfg.scenario == "separate") {
            namespace DR = Pulsar::InventorySync::DirectRestore;
            std::uint32_t sp = 0;
            const bool spOk = me.valid && DR::SkillPointsForTest(sp);
            char c[400];
            std::snprintf(c, sizeof(c),
                          ",\"sep\":{\"itemH\":\"%08X\",\"itemC\":\"%08X\",\"baseH\":%d,\"baseC\":%d,\"cntH\":%d,\"cntC\":%d,"
                          "\"baseSp\":%u,\"sp\":%d,\"perk\":\"%08X\",\"perkBefore\":%d,\"perkAfter\":%d,"
                          "\"restores\":%u,\"restoresAtAct\":%u,\"bridgeAtWallMs\":%llu,\"loadedSinceAct\":%s,\"perks\":\"",
                          s_state.sepItemH, s_state.sepItemC, s_state.sepBaseH, s_state.sepBaseC,
                          me.valid && s_state.sepItemH ? Pulsar::WorldSync::Loot::CountItemForTest(player, s_state.sepItemH) : -1,
                          me.valid && s_state.sepItemC ? Pulsar::WorldSync::Loot::CountItemForTest(player, s_state.sepItemC) : -1,
                          s_state.sepBaseSp, spOk ? static_cast<int>(sp) : -1, s_state.sepPerk, s_state.sepPerkBefore,
                          s_state.sepPerkAfter, DR::RestoresForTest(), s_state.sepRestoresAtAct,
                          static_cast<unsigned long long>(s_state.sepBridgeWallMs),
                          s_state.acted && Pulsar::WorldSync::GetLastLoadCompletedTick() != s_state.sepLoadAtAct ? "true" : "false");
            j += c;
            // Every own perk as "id:rank", so the scorer can look up the other player's.
            std::vector<std::pair<std::uint32_t, std::int8_t>> perks;
            if (me.valid) DR::PerksForTest(perks);
            for (std::size_t i = 0; i < perks.size(); ++i) {
                char e[24];
                std::snprintf(e, sizeof(e), "%s%08X:%d", i ? "," : "", perks[i].first, perks[i].second);
                j += e;
            }
            j += "\"}";
        }
        if (s_state.cfg.scenario == "damage") {
            // Own hit, and the health of every living or dead save-file NPC nearby.
            char c[200];
            std::snprintf(c, sizeof(c), ",\"dmg\":{\"target\":\"%08X\",\"hitAtWallMs\":%llu,\"before\":%.1f,\"after\":%.1f}",
                          s_state.dmgTarget, static_cast<unsigned long long>(s_state.dmgHitWallMs), s_state.dmgBefore,
                          s_state.dmgAfter);
            j += c;
            NpcSeen npcs[64];
            const int n = me.valid ? SafeListNpcs(player->parentCell, me.x, me.y, me.z, 60.0f, npcs, 64) : 0;
            j += ",\"npcs\":[";
            for (int i = 0; i < n; ++i) {
                char e[80];
                std::snprintf(e, sizeof(e), "%s{\"id\":\"%08X\",\"dead\":%s,\"health\":%.1f}", i ? "," : "", npcs[i].id,
                              npcs[i].dead ? "true" : "false", SafeHealth(npcs[i].actor));
                j += e;
            }
            j += "]";
        }
        if (s_state.cfg.scenario == "friendlyfire") {
            // Host: the partner's figure. Client: its own health and downed state.
            char c[200];
            std::snprintf(c, sizeof(c),
                          ",\"ff\":{\"figure\":%.1f,\"figureDead\":%s,\"heals\":%u,\"own\":%.1f,\"ownDowned\":%s,\"hits\":%d}",
                          cloneVisible ? SafeHealth(clone) : -1.0f, cloneVisible && SafeIsDead(clone) ? "true" : "false",
                          Pulsar::Coop::Mortality::FigureHealsForTest(), me.valid ? SafeHealth(player) : -1.0f,
                          Pulsar::Coop::Mortality::IsLocalDowned() ? "true" : "false", s_state.ffHits);
            j += c;
        }
        j += "}\n";
        WriteState(j);
    }
}

#else  // PULSAR_HARDENED: the bot does not exist in release builds.

namespace Pulsar::TestBot
{
    void Tick() {}
    bool AimOverrideForTest(float&, bool&) { return false; }
}

#endif
