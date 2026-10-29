#pragma once

// Direct sessions without a relay or lobby server (docs/SPEC-STATUS.md, phase 2).
//
// Host: gathers the endpoints a friend can reach - LAN addresses, the public address a
// STUN server reports for the game socket, and a port the router opens by UPnP - and
// publishes them with a fresh session secret as a join code (log, PULSAR_JoinCode.txt
// next to PULSAR.ini, clipboard, on-screen message).
//
// Client: from the join code, punches toward and connects to every host endpoint at once
// and keeps the first that answers. If none does within a few seconds, it publishes a
// reply code (its own public endpoint); once the host enters it (F2 pastes it from the
// clipboard, the launcher writes PULSAR_ReplyCodeIn.txt, or ReplyCode in PULSAR.ini) the
// host punches toward the client too.
//
// The session secret is mixed into the encryption key exchange, so only someone with the
// code can finish a connection.
//
// Threading: everything except the worker threads (DNS, UPnP) runs on the network thread
// while NetworkHost holds its ENet lock (Begin*, Tick, OnConnected, Intercept).

#include <enet/enet.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Pulsar::Network::Direct {

    // ENet peers a joining client needs: one connect attempt per host endpoint.
    inline constexpr std::size_t kJoinPeers = 6;

    struct Settings
    {
        std::vector<std::string> stunServers;   // "host:port"; empty = no STUN
        bool                     useUpnp = true;
        bool                     clipboard = true;   // copy published codes to the clipboard
    };

    // Host: start gathering endpoints for the ENet host bound to a_port. Makes a new secret.
    void BeginHost(ENetHost* a_host, std::uint16_t a_port, const Settings& a_settings);

    // Client: start joining with a join code. False if the code is not a valid host code.
    bool BeginJoin(ENetHost* a_host, const std::string& a_codeText, const Settings& a_settings);

    // The session secret (host: generated; client: from the code), for the key exchange.
    std::string SecretBytes();

    // Host: a reply code from the friend (text). False if invalid or for another session.
    bool SubmitReply(const std::string& a_codeText);

    // Network thread, under the ENet lock, every NetworkHost::Update.
    void Tick(ENetHost* a_host, bool a_connected);

    // Client: a connection succeeded; drop the other attempts.
    void OnConnected(ENetHost* a_host, ENetPeer* a_winner);

    // Client: true while direct connection attempts are in flight (their failures are
    // not disconnects of the session).
    bool Joining();

    // ENet intercept: consumes STUN responses. 1 = consumed.
    int ENET_CALLBACK Intercept(ENetHost* a_host, ENetEvent* a_event);

    // Published codes ("" until published).
    std::string HostCode();
    std::string ReplyCode();

    // Remove the UPnP mapping, stop retries.
    void Stop();

    // Endpoint text "a.b.c.d:port" (logs).
    std::string ToString(std::uint32_t a_ipv4NetOrder, std::uint16_t a_port);

}  // namespace Pulsar::Network::Direct
