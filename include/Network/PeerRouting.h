#pragma once

// 2-4 players (docs/SPEC-STATUS.md, phase 3): the host is the hub. Each client sends to the
// host only; the host handles a client's packet like before and forwards it, wrapped with
// the sender's player ID, to the other clients. Game-free.
//
// Player IDs: the host is 0, clients get 1-3 in the order of their slot on the host.
//
// Wire format of a forwarded packet: [PKT_FROM][sender id][the original packet].

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Network/PacketTypes.h"

namespace Pulsar::Network::Routing {

    inline constexpr std::uint8_t kHostId     = 0;
    inline constexpr std::uint8_t kMaxPlayers = 4;   // host + 3 clients
    inline constexpr std::uint8_t kNoPlayer   = 0xFF;

    enum class Class : std::uint8_t {
        kControl,   // between one client and the host only: never forwarded
        kPlayer,    // about the sending player (their figure, gear, vote ...): forwarded, receiver needs the sender
        kWorld,     // about the shared world: forwarded as is
    };

    inline Class ClassOf(std::uint8_t a_type)
    {
        switch (a_type) {
        case PKT_HANDSHAKE:
        case PKT_CLIENT_PROFILE:
        case PKT_SAVE_TRANSFER:
        case PKT_CLIENT_LOAD_COMPLETE:
        case PKT_HOST_REQUEST_RECONNECT:
        case PKT_BRIDGE_REQUEST:
        case PKT_TIME_WEATHER_REQ:
        case PKT_TIME_WEATHER:
        case PKT_CRYPTO_HELLO:
        case PKT_NPC_DAMAGE:          // the host owns NPCs and broadcasts their state
        case PKT_FROM:
        case PKT_ROSTER:
            return Class::kControl;
        case PKT_POSITION:
        case PKT_ANIMATION:
        case PKT_BONES:
        case PKT_FIRE:
        case PKT_EQUIP:
        case PKT_EQUIPMENT:
        case PKT_EQUIP_V2:
        case PKT_PLAYER_DOWNED:
        case PKT_PLAYER_REVIVE:
        case PKT_PLAYER_COMBAT:
        case PKT_VOICE:
        case PKT_TRANSIT:
        case PKT_LOOT_DROP:
        case PKT_KILL_XP:
        case PKT_QUEST_REWARD:
        case PKT_TAKEOFF_VOTE:
        case PKT_XP_GAIN:
        case PKT_VEHICLE_POS:
        case PKT_SHIP_POS:
        case PKT_FLIGHT_STATE:
        case PKT_FLIGHT_INPUT:
        case PKT_PMO_PAYLOAD:
        case PKT_SHIP_TRANSFORM:
        case PKT_SHIP_FIRE:
            return Class::kPlayer;
        default:
            return Class::kWorld;
        }
    }

    // True if the host forwards this packet type from one client to the others.
    inline bool Forwarded(std::uint8_t a_type) { return ClassOf(a_type) != Class::kControl; }

    inline std::vector<std::uint8_t> Wrap(std::uint8_t a_sender, const std::uint8_t* a_data, std::size_t a_len)
    {
        std::vector<std::uint8_t> out;
        out.reserve(a_len + 2);
        out.push_back(PKT_FROM);
        out.push_back(a_sender);
        out.insert(out.end(), a_data, a_data + a_len);
        return out;
    }

    struct Unwrapped { std::uint8_t sender; const std::uint8_t* data; std::size_t len; };

    // A forwarded packet's sender and inner packet; a plain packet comes from the host.
    // Returns sender kNoPlayer for a malformed or nested wrapper.
    inline Unwrapped Unwrap(const std::uint8_t* a_data, std::size_t a_len)
    {
        if (a_len >= 1 && a_data[0] == PKT_FROM) {
            if (a_len < 3 || a_data[1] == kHostId || a_data[1] >= kMaxPlayers || a_data[2] == PKT_FROM)
                return { kNoPlayer, a_data, 0 };
            return { a_data[1], a_data + 2, a_len - 2 };
        }
        return { kHostId, a_data, a_len };
    }

    // Roster: which players are in the session; sent by the host to each client on every
    // change. Wire: [PKT_ROSTER][your id][count][count x player id].
    inline std::vector<std::uint8_t> MakeRoster(std::uint8_t a_yourId, const std::vector<std::uint8_t>& a_ids)
    {
        std::vector<std::uint8_t> out{ PKT_ROSTER, a_yourId, static_cast<std::uint8_t>(a_ids.size()) };
        out.insert(out.end(), a_ids.begin(), a_ids.end());
        return out;
    }

    struct Roster { std::uint8_t yourId = kNoPlayer; std::vector<std::uint8_t> ids; };

    inline bool ParseRoster(const std::uint8_t* a_data, std::size_t a_len, Roster& a_out)
    {
        if (a_len < 3 || a_data[0] != PKT_ROSTER) return false;
        const std::size_t n = a_data[2];
        if (n > kMaxPlayers || a_len != 3 + n || a_data[1] == kHostId || a_data[1] >= kMaxPlayers) return false;
        a_out.yourId = a_data[1];
        a_out.ids.assign(a_data + 3, a_data + 3 + n);
        for (auto id : a_out.ids) if (id >= kMaxPlayers) return false;
        return true;
    }

}  // namespace Pulsar::Network::Routing
