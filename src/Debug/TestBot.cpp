#include "PCH.h"
#include "Debug/TestBot.h"

#ifndef PULSAR_HARDENED

#include "Config.h"
#include "InventorySync/DirectRestore.h"
#include "Network/NetworkHost.h"
#include "RemotePlayer/RemotePlayerManager.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

using namespace std::string_view_literals;

namespace Pulsar::TestBot
{
    namespace
    {
        struct Settings {
            bool        enabled = false;
            std::string stateFile;          // where the state JSON goes (test slot logs)
            std::uint64_t moveAfterMs = 15000;   // host: clone visible this long, then move
            float       moveMetres = 5.0f;  // host: X offset of the test move (game units = metres)
        };

        struct State {
            bool          loaded = false;
            Settings      cfg;
            std::uint64_t nextWriteMs = 0;
            std::uint64_t cloneSeenSinceMs = 0;
            bool          moved = false;
            std::uint64_t movedAtWallMs = 0;
            float         moveTarget[3]{};
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

        // Write to a temp file, then replace: the launcher never reads half a file.
        void WriteState(const std::string& a_json)
        {
            const std::string tmp = s_state.cfg.stateFile + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out) return;
                out << a_json;
            }
            MoveFileExA(tmp.c_str(), s_state.cfg.stateFile.c_str(), MOVEFILE_REPLACE_EXISTING);
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

        // The test action: the host moves itself once the partner is settled.
        if (isHost && !s_state.moved && cloneVisible && me.valid &&
            now - s_state.cloneSeenSinceMs >= s_state.cfg.moveAfterMs) {
            s_state.moveTarget[0] = me.x + s_state.cfg.moveMetres;
            s_state.moveTarget[1] = me.y;
            s_state.moveTarget[2] = me.z;
            const bool ok = SafeMovePlayer(player, s_state.moveTarget[0], s_state.moveTarget[1], s_state.moveTarget[2]);
            s_state.moved = true;
            s_state.movedAtWallMs = WallMs();
            char msg[160];
            std::snprintf(msg, sizeof(msg), "host moved %.1f m on X -> (%.2f,%.2f,%.2f) ok=%d",
                          s_state.cfg.moveMetres, s_state.moveTarget[0], s_state.moveTarget[1],
                          s_state.moveTarget[2], ok ? 1 : 0);
            const std::string line(msg);
            PLOG_INFO("[PULSAR] TestBot: {}"sv, line);
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
        j += std::string(",\"moved\":") + (s_state.moved ? "true" : "false");
        j += ",\"movedAtWallMs\":" + std::to_string(s_state.movedAtWallMs);
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
