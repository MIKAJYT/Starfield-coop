#pragma once

#include <enet/enet.h>
#include <cstdint>
#include <functional>
#include <string>
#include <filesystem>
#include <mutex>
#include <vector>
#include "Network/Crypto.h"

namespace Pulsar::Network
{
    constexpr uint16_t DEFAULT_PORT = 7777;
    constexpr uint32_t MAX_CLIENTS = 3;   // 2-4 players: the host plus up to 3 clients (PULSAR.ini MaxPlayers)
    constexpr uint32_t NUM_CHANNELS = 3;  // 0=control/state, 1=bulk(saves), 2=voice(unreliable). Voice protocol change, both peers must match.

    class NetworkHost
    {
    public:
        ~NetworkHost();

        bool StartHost(uint16_t a_port = DEFAULT_PORT);
        bool Connect(const char* a_address, uint16_t a_port = DEFAULT_PORT);
        // Enable relay mode: register/join via the relay instead of a direct
        // peer. a_relayAddr = "host:port", a_sessionHex = 32 hex chars. Call
        // BEFORE StartHost/Connect. No-op if either is empty (direct mode).
        void SetRelay(const std::string& a_relayAddr, const std::string& a_sessionHex);
        // Per-lobby E2E auth secret (anti-MITM), delivered over the trusted DB. Stored now,
        // applied to the crypto channel at connect (before the key exchange) when the
        // e2eRelayAuth config is on. Empty = none. Call BEFORE StartHost/Connect.
        void SetAuthSecret(const std::string& a_secret);
        void Update();
        void Send(const uint8_t* a_data, size_t a_length, bool a_reliable = true);
        // Send one already-framed voice payload ([VoiceFrameHeader][opus]) to the
        // peer on channel 2, unreliable. Prepends PKT_VOICE and encrypts via EncSend.
        // No-op if not connected. Called from VoiceBridge on the network thread.
        void SendVoiceFrame(const uint8_t* a_payload, size_t a_len);
        // Send a specific .sfs file to client. If path empty, uses GetMostRecentSavePath().
        void SendSaveToClient(const std::string& a_path = {});
        // Host-side: trigger quicksave, poll until new save is written, then send it.
        // a_target: the player who loads it (a joining one), 0xFF = every client.
        void BeginHostQuicksaveTransfer(uint8_t a_target = 0xFF);
        // L.7 V8b - host-side: trigger a CUSTOM-named save (mode=1 via
        // SaveLoadManager dispatch REL::ID 98640) instead of a quicksave,
        // poll until the new file appears, then send it. The point is that
        // engine-side, a regular save load goes through the full state-init
        // path (same code as game-start save load), while a quicksave load
        // hits an optimised path that breaks for 2+ mid-session loads.
        void BeginHostBridgeSaveTransfer(const char* a_saveName);
        void Shutdown();
        void Disconnect();  // Graceful disconnect (client or host)
        void SetPeerTimeout(uint32_t a_ms);
        void ResetTeleport();
        // Phase 3i v17.1 - suppress next ResetTeleport call. Used by DockSync
        // forward-bridge: the save itself places client at host's position so
        // the auto-teleport that fires on the next PKT_POSITION would push
        // them OUT of the cockpit cell into NULL state. DockSync sets this
        // before triggering BeginHostQuicksaveTransfer; ResetTeleport checks
        // and clears it.
        void SuppressNextResetTeleport();

        bool IsHost() const { return m_isHost; }
        bool IsConnected() const { return m_peer != nullptr; }
        bool IsActive() const { return m_host != nullptr; }
        // L.7 V9.4 - true only after ENet's OnConnect callback fires (peer
        // fully established). enet_host_connect returns a peer in CONNECTING
        // state immediately; sends queued before the callback can be silently
        // dropped. The handshake-on-reconnect bug was caused by the client's
        // tick calling ClientSendHandshake while peer was still CONNECTING.
        bool IsFullyConnected() const {
            return m_connectionEstablished.load(std::memory_order_acquire);
        }

        // Game-thread accessors for pending-teleport state. Network thread sets
        // these when a position packet first arrives; game thread applies them
        // via SetPosition (which is game-thread-only).
        bool ConsumePendingTeleport(float& a_x, float& a_y, float& a_z, float& a_rotZ);

        // 2-4 players (Network/PeerRouting.h). This player's ID (host 0, clients 1-3;
        // 0xFF on a client until the host's roster arrives), how many players are in the
        // session, and the sender of the packet being handled (network thread, inside a
        // packet handler).
        uint8_t LocalPlayerId() const { return m_localPlayerId.load(std::memory_order_acquire); }
        uint8_t PlayerCount() const { return m_playerCount.load(std::memory_order_acquire); }
        static uint8_t CurrentSender();

    private:
        void OnConnect(ENetPeer* a_peer);
        void OnReceive(ENetPeer* a_peer, ENetPacket* a_packet, uint8_t a_channel = 0);
        void OnDisconnect(ENetPeer* a_peer, uint32_t a_reason = 0);

        // One connection: the host keeps one per client (players 1-3), a client one for
        // the host. Each has its own E2E key exchange.
        struct PendingTx { int channel; std::vector<uint8_t> data; bool reliable; };
        struct PeerSlot
        {
            ENetPeer*              peer = nullptr;
            uint8_t                id = 0;              // player ID of the other side
            CryptoChannel          crypto;
            bool                   cryptoReady = false; // shared key derived (peer hello received)
            std::vector<PendingTx> preCryptoTx;         // app packets sent before the key is ready
            bool                   decryptedAny = false;
            int                    decryptFailures = 0;
            uint64_t               lastAutoBridgeMs = 0; // see the handshake debounce (L.7 D1)
        };
        static constexpr size_t kSlots = 3;
        PeerSlot  m_slots[kSlots];
        PeerSlot* SlotOf(ENetPeer* a_peer);
        PeerSlot* AllocSlot(ENetPeer* a_peer);
        PeerSlot* SlotById(uint8_t a_id);
        void      FreeSlot(PeerSlot& a_slot);
        void      SendRoster();            // host: to every client
        bool      IsPrimary(const PeerSlot& a_slot) const { return a_slot.peer && a_slot.peer == m_peer; }

        // E2E crypto. EncSend encrypts (or queues until the handshake completes) then
        // sends to every connected slot; EncSendSlot to one. RawSendUnenc bypasses crypto
        // (used only for the key-exchange hello).
        void EncSend(int a_channel, const uint8_t* a_data, size_t a_len, bool a_reliable);
        void EncSendSlot(PeerSlot& a_slot, int a_channel, const uint8_t* a_data, size_t a_len, bool a_reliable);
        void RawSendUnenc(PeerSlot& a_slot, int a_channel, const uint8_t* a_data, size_t a_len, bool a_reliable);
        void SendCryptoHello(PeerSlot& a_slot);
        void FlushPreCryptoTx(PeerSlot& a_slot);

        std::vector<uint8_t> m_authSecret;   // DB-delivered E2E auth secret bytes (anti-MITM); empty = none
        bool          m_e2e = true;          // cached from config at connect
        // Wrong session secret: nothing ever decrypts on the connection (OnReceive).
        static constexpr int kWrongKeyFailures = 20;
        bool          m_wrongKey = false;    // disconnected for it: no auto-reconnect
        // Guards m_e2e and each slot's cryptoReady / preCryptoTx ONLY. EncSend runs from both
        // the game thread (Send) and the network thread (quicksave SendSaveToClient), and the
        // SendSaveToClient paths do not hold m_enetMutex, so that lock alone does not
        // serialize the queue. This dedicated lock does, and is always innermost: never
        // held across an enet_* or crypto call, so it cannot deadlock with m_enetMutex.
        mutable std::mutex m_cryptoQueueMtx;

        std::atomic<uint8_t> m_localPlayerId{ 0xFF };
        std::atomic<uint8_t> m_playerCount{ 1 };
        uint8_t              m_saveTarget = 0xFF;   // host: the player the next save goes to (0xFF = every client)

        ENetHost* m_host = nullptr;
        ENetPeer* m_peer = nullptr;
        bool      m_isHost = false;
        bool      m_initialized = false;

        // Relay mode (NAT-free). Empty session = direct mode (unchanged path).
        bool        m_relayMode = false;
        ENetAddress m_relayAddress{};
        uint8_t     m_relaySession[16] = {};
        uint64_t    m_relayKeepaliveAt = 0;   // GetTickCount64() of next REGISTER
        void        SendRelayControl(uint8_t a_type);  // sends via m_host->socket

        // P2P hole-punch rendezvous (direct-first, relay-fallback).
        enum class Rz { kNone, kAwaitPeer, kDirectConnecting, kDoneDirect, kDoneRelay };
        Rz          m_rz = Rz::kNone;
        uint64_t    m_rzDeadline = 0;
        bool        m_forceRelay = false;     // PULSAR_FORCE_RELAY=1 → relay-always
        ENetAddress m_directAddress{};        // peer's punched public endpoint

        // Direct session by join code (DirectSession.h): host publishes a code, client joins by it.
        bool        m_directHost = false;
        bool        m_directJoin = false;
        void        PunchTo(const ENetAddress& a_ep, int a_count);  // raw datagrams via m_host->socket

        bool      m_pendingSendPos = false;

        // Protects all ENet API calls. enet_peer_send / enet_host_service are NOT
        // thread-safe; without this lock, sends from the game thread race with
        // the network thread's service loop and corrupt internal state.
        mutable std::mutex m_enetMutex;

        // Client: auto-reconnect state
        std::string m_lastAddress;
        uint16_t    m_lastPort = DEFAULT_PORT;
        uint64_t    m_reconnectAt = 0;  // tick to attempt reconnect (0=disabled)

        // Client: pending teleport to host position
        struct TeleportData { float x, y, z, rotX, rotY, rotZ; };
        TeleportData m_pendingTeleport{};
        bool         m_hasPendingTeleport = false;
        bool         m_clientTeleported = false;
        // Phase 3i v17.1: bridge saves don't need auto-teleport (save itself
        // places client correctly). DockSync sets this flag before triggering
        // a forward/return bridge.
        std::atomic<bool> m_suppressNextResetTeleport{ false };
        // L.7 V9: set by network-receive thread on PKT_HOST_REQUEST_RECONNECT,
        // observed + cleared by game-thread tick which then calls Disconnect()
        // safely (game-thread context).
        std::atomic<bool> m_pendingReconnect{ false };
        // L.7 V9.4 - set on OnConnect, cleared on OnDisconnect. Lets callers
        // (e.g. ClientSendHandshake) wait for the peer to be ENET_PEER_STATE_
        // CONNECTED instead of just checking m_peer != null.
        std::atomic<bool> m_connectionEstablished{ false };

        // L.7 V3.1 - pending-Connect queue. Connect() called while the
        // player isn't fully spawned (main menu, no save loaded) stashes the
        // address+port here and returns true. Update() drains it on the
        // first tick where IsPlayerReadyForConnect() reports true. Lets the
        // SFSE plugin-load auto-connect "wait" through the save load instead
        // of being refused outright. UI Connect button rides the same path.
        std::atomic<bool> m_pendingConnectActive{ false };
        std::mutex        m_pendingConnectMu;
        std::string       m_pendingConnectAddress;
        uint16_t          m_pendingConnectPort = DEFAULT_PORT;
        // Helper: actual ENet host create + connect logic. Public Connect()
        // either calls this immediately (player ready) or stashes the
        // arguments and returns; Update() drains the queue.
        bool DoConnectInternal(const char* a_address, uint16_t a_port);

        // Host: pending quicksave transfer state
        bool        m_quicksavePending = false;
        uint64_t    m_quicksaveStartTick = 0;     // GetTickCount64() at trigger
        uint64_t    m_quicksaveLastPoll = 0;
        std::filesystem::file_time_type m_quicksavePreTime{};  // mtime of newest save BEFORE trigger
        std::string m_quicksaveCandidate;          // path of newest save that appeared after trigger
        uintmax_t   m_quicksaveCandidateSize = 0;  // last observed size
        uint64_t    m_quicksaveSizeStableSince = 0;

        // L.7 D1 - debounce the auto-bridge fired on every PKT_HANDSHAKE (per player:
        // PeerSlot::lastAutoBridgeMs). A lossy ENet link (timeout 5s) re-handshakes after
        // a hiccup; without it, the host re-sends the bridge save to a client that is
        // ALREADY in-world, forcing a save-reload mid-session → clone freed mid-load →
        // CTD. We only auto-bridge once per window; a real rejoin can F8.
    };

    NetworkHost& GetSingleton();
}
