#include "PCH.h"
#include "Debug/TestBot.h"

#ifndef PULSAR_HARDENED

#include "Config.h"
#include "InventorySync/DirectRestore.h"
#include "NPCSync/NPCSync.h"
#include "Network/NetworkHost.h"
#include "QuestSync/RewardShare.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "SpaceSync/SpaceSync.h"
#include "Coop/TakeoffVote.h"
#include "UI/HudMessage.h"
#include "TransitSync/TransitSync.h"
#include "WorldSync/LootSync.h"

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
        struct Settings {
            bool        enabled = false;
            std::string stateFile;          // state log, one JSON line per second (test slot logs)
            std::string scenario = "follow";
            std::uint64_t moveAfterMs = 45000;   // host: clone visible this long, then act
            float       moveMetres = 5.0f;  // follow: X offset of the test move (game units = metres)
            float       doorRadius = 60.0f; // door: search radius for a load door (metres)
            float       lootRadius = 30.0f; // loot: search radius for the NPC to kill (metres)
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
            int           notifySent = 0;        // message check: test messages sent
        };
        State s_state;

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
        if (!isHost && s_state.cfg.scenario == "takeoff" && !s_state.voteReadyWallMs) {
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
        if (!isHost && s_state.cfg.scenario == "takeoff" && s_state.cfg.takeoffPartnerOutside &&
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
        if (isHost && s_state.cfg.scenario == "takeoff" && (!s_state.acted || f8Pending) && me.valid &&
            (s_state.takeoffStep != 0 || takeoffDue)) {
            TickTakeoff(player, me, now);
        }

        // The test action, once the partner is settled: host only.
        if (isHost && s_state.cfg.scenario != "loot" && s_state.cfg.scenario != "takeoff" &&
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
        if (s_state.cfg.scenario == "takeoff") {
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
        j += "}\n";
        WriteState(j);
    }
}

#else  // PULSAR_HARDENED: the bot does not exist in release builds.

namespace Pulsar::TestBot
{
    void Tick() {}
}

#endif
