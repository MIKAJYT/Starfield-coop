#include "PCH.h"
#include "Debug/TestBot.h"

#ifndef PULSAR_HARDENED

#include "Config.h"
#include "InventorySync/DirectRestore.h"
#include "Network/NetworkHost.h"
#include "RemotePlayer/RemotePlayerManager.h"
#include "TransitSync/TransitSync.h"

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
        struct Settings {
            bool        enabled = false;
            std::string stateFile;          // state log, one JSON line per second (test slot logs)
            std::string scenario = "follow";
            std::uint64_t moveAfterMs = 45000;   // host: clone visible this long, then act
            float       moveMetres = 5.0f;  // follow: X offset of the test move (game units = metres)
            float       doorRadius = 60.0f; // door: search radius for a load door (metres)
        };

        struct State {
            bool          loaded = false;
            Settings      cfg;
            std::uint64_t nextWriteMs = 0;
            std::uint64_t cloneSeenSinceMs = 0;
            bool          acted = false;
            std::uint64_t actedAtWallMs = 0;
            std::string   actionInfo;
            int           writes = 0;
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
        } else {
            s_state.cloneSeenSinceMs = 0;
        }

        // The test action, once the partner is settled: host only.
        if (isHost && !s_state.acted && cloneVisible && me.valid &&
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
        j += ",\"scenario\":\"" + s_state.cfg.scenario + "\"";
        j += std::string(",\"acted\":") + (s_state.acted ? "true" : "false");
        j += ",\"actedAtWallMs\":" + std::to_string(s_state.actedAtWallMs);
        j += ",\"actionInfo\":\"" + s_state.actionInfo + "\"";
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
