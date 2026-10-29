#include "PCH.h"
#include "Network/DirectSession.h"

#include "Config.h"
#include "Network/JoinCode.h"
#include "Network/Stun.h"
#include "UI/HudMessage.h"
#include "Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <natupnp.h>
#include <bcrypt.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "bcrypt.lib")

using namespace std::string_view_literals;

namespace Pulsar::Network::Direct {

    namespace {
        namespace JC = Pulsar::Network::JoinCode;

        constexpr std::uint64_t kStunResendMs      = 500;
        constexpr std::uint64_t kStunGiveUpMs      = 3000;
        constexpr std::uint64_t kUpnpGiveUpMs      = 8000;
        constexpr std::uint64_t kAttemptEveryMs    = 4000;
        constexpr std::uint64_t kReplyAfterMs      = 6000;
        constexpr std::uint64_t kJoinGiveUpMs      = 10 * 60 * 1000;
        constexpr std::uint64_t kReplyPunchForMs   = 120 * 1000;
        constexpr std::uint64_t kReplyPunchEveryMs = 1000;
        constexpr int           kPunchCount        = 3;

        enum class Role { kNone, kHost, kClient };

        struct State
        {
            Role                               role = Role::kNone;
            Settings                           cfg;
            std::array<std::uint8_t, JC::kSecretBytes> secret{};
            std::uint16_t                      port = 0;   // our bound port
            std::uint64_t                      beganMs = 0;
            std::vector<JC::Endpoint>          lan;

            // STUN: servers resolved by a worker thread, then asked from the game socket.
            std::vector<ENetAddress>           stunAddrs;       // guarded by mu
            std::atomic<bool>                  stunResolved{ false };
            Stun::TransactionId                tx{};
            bool                               stunHave = false;
            JC::Endpoint                       stunEp;
            std::uint64_t                      stunNextSend = 0;

            // UPnP (host): 0 pending, 1 mapped, 2 failed or off.
            std::atomic<int>                   upnpState{ 2 };
            JC::Endpoint                       upnpEp;          // guarded by mu
            std::uint16_t                      upnpExtPort = 0; // guarded by mu
            std::wstring                       upnpClient;      // LAN address the mapping points at

            // Host: endpoints from a reply code to punch toward.
            std::vector<JC::Endpoint>          replyTargets;    // guarded by mu
            std::uint64_t                      replyUntilMs = 0;
            std::uint64_t                      replyNextPunch = 0;
            int                                lastPasteKey = 0;

            // Client.
            std::vector<JC::Endpoint>          targets;
            std::uint64_t                      nextAttemptMs = 0;
            int                                attempts = 0;
            bool                               connected = false;

            bool                               published = false;
            std::string                        hostCode;        // guarded by mu
            std::string                        replyCode;       // guarded by mu
            std::mutex                         mu;
        };

        State s;

        std::uint64_t Now() { return ::GetTickCount64(); }

        void RandomBytes(std::uint8_t* a_out, std::size_t a_n)
        {
            ::BCryptGenRandom(nullptr, a_out, static_cast<ULONG>(a_n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        }

        bool UsableIpv4(std::uint32_t a_netOrder)
        {
            const std::uint8_t a = a_netOrder & 0xFF, b = (a_netOrder >> 8) & 0xFF;
            if (a == 0 || a == 127 || a >= 224) return false;
            if (a == 169 && b == 254) return false;   // link-local
            return true;
        }

        // Up, non-loopback IPv4 addresses; adapters with a default gateway first.
        std::vector<std::uint32_t> LanAddresses(std::wstring* a_gatewayAdapterIp)
        {
            std::vector<std::uint32_t> withGw, without;
            ULONG size = 16 * 1024;
            std::vector<std::uint8_t> buf(size);
            ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
            auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
            ULONG r = ::GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
            if (r == ERROR_BUFFER_OVERFLOW) {
                buf.resize(size);
                aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
                r = ::GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
            }
            if (r != NO_ERROR) return {};
            for (auto* a = aa; a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                    if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                    const auto ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.S_un.S_addr;
                    if (!UsableIpv4(ip)) continue;
                    if (a->FirstGatewayAddress) {
                        if (withGw.empty() && a_gatewayAdapterIp) {
                            wchar_t w[INET_ADDRSTRLEN] = {};
                            in_addr ia{};
                            ia.S_un.S_addr = ip;
                            ::InetNtopW(AF_INET, &ia, w, INET_ADDRSTRLEN);
                            *a_gatewayAdapterIp = w;
                        }
                        withGw.push_back(ip);
                    } else {
                        without.push_back(ip);
                    }
                }
            }
            withGw.insert(withGw.end(), without.begin(), without.end());
            return withGw;
        }

        void ResolveStunServers(std::vector<std::string> a_servers)
        {
            std::vector<ENetAddress> out;
            for (const auto& hp : a_servers) {
                const auto colon = hp.rfind(':');
                const std::string host = colon == std::string::npos ? hp : hp.substr(0, colon);
                const std::string port = colon == std::string::npos ? "3478" : hp.substr(colon + 1);
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                addrinfo* res = nullptr;
                if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
                    PLOG_INFO("[PULSAR] [Direct] STUN server {} did not resolve"sv, hp);
                    continue;
                }
                const auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
                ENetAddress e{};
                e.host = sin->sin_addr.S_un.S_addr;
                e.port = ntohs(sin->sin_port);
                out.push_back(e);
                ::freeaddrinfo(res);
            }
            {
                std::lock_guard lk(s.mu);
                s.stunAddrs = std::move(out);
            }
            s.stunResolved.store(true, std::memory_order_release);
        }

        // Ask the router to forward UDP a_port to this PC (UPnP IGD, Windows' own client).
        void UpnpMap(std::uint16_t a_port, std::wstring a_client)
        {
            if (a_client.empty()) { s.upnpState.store(2); PLOG_INFO("[PULSAR] [Direct] UPnP: no LAN address with a gateway"sv); return; }
            const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IUPnPNAT* nat = nullptr;
            IStaticPortMappingCollection* col = nullptr;
            bool ok = false;
            if (SUCCEEDED(::CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat))) && nat &&
                SUCCEEDED(nat->get_StaticPortMappingCollection(&col)) && col) {
                BSTR proto = ::SysAllocString(L"UDP");
                BSTR client = ::SysAllocString(a_client.c_str());
                BSTR desc = ::SysAllocString(L"PULSAR co-op");
                for (std::uint16_t ext = a_port; ext < a_port + 4 && !ok; ++ext) {
                    col->Remove(ext, proto);   // a stale mapping of ours from a crashed session
                    IStaticPortMapping* map = nullptr;
                    if (SUCCEEDED(col->Add(ext, proto, a_port, client, VARIANT_TRUE, desc, &map)) && map) {
                        BSTR extIp = nullptr;
                        if (SUCCEEDED(map->get_ExternalIPAddress(&extIp)) && extIp) {
                            in_addr ia{};
                            if (::InetPtonW(AF_INET, extIp, &ia) == 1 && UsableIpv4(ia.S_un.S_addr)) {
                                std::lock_guard lk(s.mu);
                                s.upnpEp = { JC::Via::kMapped, ia.S_un.S_addr, ext };
                                s.upnpExtPort = ext;
                                ok = true;
                            }
                            ::SysFreeString(extIp);
                        }
                        map->Release();
                    }
                }
                ::SysFreeString(proto);
                ::SysFreeString(client);
                ::SysFreeString(desc);
            }
            if (col) col->Release();
            if (nat) nat->Release();
            if (SUCCEEDED(init)) ::CoUninitialize();
            s.upnpState.store(ok ? 1 : 2, std::memory_order_release);
            if (ok) {
                std::lock_guard lk(s.mu);
                PLOG_DIAG("[PULSAR] co-op: the router opened a port automatically (UPnP): {}"sv, ToString(s.upnpEp.ipv4, s.upnpEp.port));
            } else {
                PLOG_DIAG("[PULSAR] co-op: the router did not open a port automatically (UPnP off or not supported)"sv);
            }
        }

        void UpnpUnmap(std::uint16_t a_ext)
        {
            const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IUPnPNAT* nat = nullptr;
            IStaticPortMappingCollection* col = nullptr;
            if (SUCCEEDED(::CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat))) && nat &&
                SUCCEEDED(nat->get_StaticPortMappingCollection(&col)) && col) {
                BSTR proto = ::SysAllocString(L"UDP");
                col->Remove(a_ext, proto);
                ::SysFreeString(proto);
                col->Release();
            }
            if (nat) nat->Release();
            if (SUCCEEDED(init)) ::CoUninitialize();
        }

        void SetClipboardText(const std::string& a_text)
        {
            if (!::OpenClipboard(nullptr)) return;
            ::EmptyClipboard();
            const std::size_t bytes = (a_text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                auto* w = static_cast<wchar_t*>(::GlobalLock(mem));
                for (std::size_t i = 0; i < a_text.size(); ++i) w[i] = static_cast<wchar_t>(static_cast<unsigned char>(a_text[i]));
                w[a_text.size()] = 0;
                ::GlobalUnlock(mem);
                if (!::SetClipboardData(CF_UNICODETEXT, mem)) ::GlobalFree(mem);
            }
            ::CloseClipboard();
        }

        std::string GetClipboardText()
        {
            std::string out;
            if (!::OpenClipboard(nullptr)) return out;
            if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* w = static_cast<const wchar_t*>(::GlobalLock(h))) {
                    for (std::size_t i = 0; w[i] && i < 512; ++i) out.push_back(w[i] < 128 ? static_cast<char>(w[i]) : '?');
                    ::GlobalUnlock(h);
                }
            }
            ::CloseClipboard();
            return out;
        }

        // Log, file next to PULSAR.ini, clipboard, on-screen message.
        void Publish(const std::string& a_code, bool a_reply, const char* a_file, const char* a_screen)
        {
            if (a_reply) PLOG_DIAG("[PULSAR] co-op: can't reach the host directly yet - reply code for the host: {}"sv, a_code);
            else         PLOG_DIAG("[PULSAR] co-op: join code (send it to your friends): {}"sv, a_code);
            std::ofstream f(Pulsar::GetPluginDir() + a_file, std::ios::trunc);
            if (f) f << a_code << "\n";
            if (s.cfg.clipboard) SetClipboardText(a_code);
            Pulsar::UI::HudMessage::Show(a_screen);
        }

        std::vector<JC::Endpoint> GatheredEndpoints()
        {
            std::vector<JC::Endpoint> e;
            {
                std::lock_guard lk(s.mu);
                if (s.upnpState.load() == 1) e.push_back(s.upnpEp);
            }
            if (s.stunHave) e.push_back(s.stunEp);
            for (const auto& l : s.lan) if (e.size() < JC::kMaxEndpoints) e.push_back(l);
            return e;
        }

        void Punch(ENetHost* a_host, const JC::Endpoint& a_ep)
        {
            ENetAddress to{};
            to.host = a_ep.ipv4;
            to.port = a_ep.port;
            std::uint8_t one = 0x50;
            ENetBuffer b{};
            b.data = &one;
            b.dataLength = 1;
            for (int i = 0; i < kPunchCount; ++i) enet_socket_send(a_host->socket, &to, &b, 1);
        }

        void TickStun(ENetHost* a_host, std::uint64_t a_now)
        {
            if (s.stunHave || !s.stunResolved.load(std::memory_order_acquire)) return;
            if (a_now - s.beganMs > kStunGiveUpMs || a_now < s.stunNextSend) return;
            s.stunNextSend = a_now + kStunResendMs;
            const auto req = Stun::MakeBindingRequest(s.tx);
            ENetBuffer b{};
            b.data = const_cast<std::uint8_t*>(req.data());
            b.dataLength = req.size();
            std::lock_guard lk(s.mu);
            for (const auto& a : s.stunAddrs) enet_socket_send(a_host->socket, &a, &b, 1);
        }

        void ConnectAttempts(ENetHost* a_host)
        {
            for (std::size_t i = 0; i < a_host->peerCount; ++i) {
                ENetPeer* p = &a_host->peers[i];
                if (p->state != ENET_PEER_STATE_DISCONNECTED && p->state != ENET_PEER_STATE_CONNECTED) enet_peer_reset(p);
            }
            for (const auto& t : s.targets) {
                Punch(a_host, t);
                ENetAddress to{};
                to.host = t.ipv4;
                to.port = t.port;
                enet_host_connect(a_host, &to, 3, 0);
            }
            ++s.attempts;
            PLOG_INFO("[PULSAR] [Direct] connect attempt {} to {} host endpoint(s)"sv, s.attempts, s.targets.size());
        }

        void Begin(Role a_role, ENetHost* a_host, const Settings& a_settings)
        {
            s.role = a_role;
            s.cfg = a_settings;
            s.beganMs = Now();
            s.published = false;
            s.stunHave = false;
            s.stunNextSend = 0;
            s.connected = false;
            s.attempts = 0;
            RandomBytes(s.tx.data(), s.tx.size());
            {
                std::lock_guard lk(s.mu);
                s.hostCode.clear();
                s.replyCode.clear();
                s.replyTargets.clear();
            }
            ENetAddress bound{};
            if (enet_socket_get_address(a_host->socket, &bound) == 0) s.port = bound.port;
            a_host->intercept = &Intercept;

            s.upnpClient.clear();
            s.lan.clear();
            for (auto ip : LanAddresses(&s.upnpClient)) {
                if (s.lan.size() >= 2) break;
                s.lan.push_back({ JC::Via::kLan, ip, s.port });
            }

            s.stunResolved.store(false);
            if (!s.cfg.stunServers.empty()) std::thread(ResolveStunServers, s.cfg.stunServers).detach();
        }
    }

    std::string ToString(std::uint32_t a_ip, std::uint16_t a_port)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", a_ip & 0xFF, (a_ip >> 8) & 0xFF, (a_ip >> 16) & 0xFF, (a_ip >> 24) & 0xFF, a_port);
        return buf;
    }

    void BeginHost(ENetHost* a_host, std::uint16_t a_port, const Settings& a_settings)
    {
        Begin(Role::kHost, a_host, a_settings);
        if (!s.port) s.port = a_port;
        RandomBytes(s.secret.data(), s.secret.size());
        if (s.cfg.useUpnp) {
            s.upnpState.store(0);
            std::thread(UpnpMap, s.port, s.upnpClient).detach();
        } else {
            s.upnpState.store(2);
        }
        PLOG_DIAG("[PULSAR] co-op: direct session on port {}, finding the addresses a friend can reach"sv, s.port);
    }

    bool BeginJoin(ENetHost* a_host, const std::string& a_codeText, const Settings& a_settings)
    {
        const auto code = JC::Decode(a_codeText);
        if (!code || code->kind != JC::Kind::kHost || code->endpoints.empty()) {
            PLOG_DIAG("[PULSAR] co-op: the join code is not valid - check it for typos"sv);
            Pulsar::UI::HudMessage::Show("Co-op: the join code is not valid. Check it for typos.");
            return false;
        }
        Begin(Role::kClient, a_host, a_settings);
        s.secret = code->secret;
        s.targets = code->endpoints;
        s.upnpState.store(2);
        for (const auto& t : s.targets)
            PLOG_INFO("[PULSAR] [Direct] host endpoint {} (via {})"sv, ToString(t.ipv4, t.port), static_cast<int>(t.via));
        ConnectAttempts(a_host);
        s.nextAttemptMs = Now() + kAttemptEveryMs;
        PLOG_DIAG("[PULSAR] co-op: joining by code, trying {} address(es) of the host"sv, s.targets.size());
        return true;
    }

    std::string SecretBytes()
    {
        return std::string(reinterpret_cast<const char*>(s.secret.data()), s.secret.size());
    }

    bool SubmitReply(const std::string& a_codeText)
    {
        const auto code = JC::Decode(a_codeText);
        if (!code || code->kind != JC::Kind::kReply) {
            PLOG_DIAG("[PULSAR] co-op: that is not a valid reply code"sv);
            Pulsar::UI::HudMessage::Show("Co-op: that is not a valid reply code.");
            return false;
        }
        if (code->tag != JC::TagOf(s.secret)) {
            PLOG_DIAG("[PULSAR] co-op: that reply code is for another session"sv);
            Pulsar::UI::HudMessage::Show("Co-op: that reply code is for another session.");
            return false;
        }
        {
            std::lock_guard lk(s.mu);
            s.replyTargets = code->endpoints;
        }
        s.replyUntilMs = Now() + kReplyPunchForMs;
        s.replyNextPunch = 0;
        PLOG_DIAG("[PULSAR] co-op: reply code accepted, opening the way to {} address(es) of your friend"sv, code->endpoints.size());
        Pulsar::UI::HudMessage::Show("Co-op: reply code accepted. Your friend should connect in a few seconds.");
        return true;
    }

    void Tick(ENetHost* a_host, bool a_connected)
    {
        if (s.role == Role::kNone || !a_host) return;
        const auto now = Now();
        TickStun(a_host, now);

        if (s.role == Role::kHost) {
            const bool stunDone = s.stunHave || s.cfg.stunServers.empty() || now - s.beganMs > kStunGiveUpMs;
            const int  upnp = s.upnpState.load(std::memory_order_acquire);
            const bool upnpDone = upnp != 0 || now - s.beganMs > kUpnpGiveUpMs;
            if (!s.published && stunDone && upnpDone) {
                s.published = true;
                JC::Code c;
                c.kind = JC::Kind::kHost;
                c.secret = s.secret;
                c.endpoints = GatheredEndpoints();
                const auto text = JC::Encode(c);
                {
                    std::lock_guard lk(s.mu);
                    s.hostCode = text;
                }
                for (const auto& e : c.endpoints)
                    PLOG_INFO("[PULSAR] [Direct] reachable at {} (via {})"sv, ToString(e.ipv4, e.port), static_cast<int>(e.via));
                Publish(text, false, "PULSAR_JoinCode.txt",
                        s.cfg.clipboard ? "Co-op: join code copied to the clipboard. Send it to your friends."
                                        : "Co-op: your join code is in PULSAR_JoinCode.txt. Send it to your friends.");
            }

            // F2 (HotkeyPasteReplyCode): take a reply code from the clipboard.
            const int key = Pulsar::GetConfig().hotkeyPasteReplyCode;
            if (key) {
                const int down = (::GetAsyncKeyState(key) & 0x8000) ? 1 : 0;
                if (down && !s.lastPasteKey) SubmitReply(GetClipboardText());
                s.lastPasteKey = down;
            }

            std::vector<JC::Endpoint> reply;
            {
                std::lock_guard lk(s.mu);
                reply = s.replyTargets;
            }
            if (!reply.empty() && now < s.replyUntilMs && now >= s.replyNextPunch) {
                s.replyNextPunch = now + kReplyPunchEveryMs;
                for (const auto& r : reply) Punch(a_host, r);
            }
            return;
        }

        // Client.
        if (a_connected || s.connected) return;
        if (now - s.beganMs > kJoinGiveUpMs) return;
        if (now >= s.nextAttemptMs) {
            s.nextAttemptMs = now + kAttemptEveryMs;
            ConnectAttempts(a_host);
        }
        const bool stunDone = s.stunHave || s.cfg.stunServers.empty() || now - s.beganMs > kStunGiveUpMs;
        if (!s.published && stunDone && now - s.beganMs > kReplyAfterMs) {
            s.published = true;
            JC::Code r;
            r.kind = JC::Kind::kReply;
            r.tag = JC::TagOf(s.secret);
            r.endpoints = GatheredEndpoints();
            const auto text = JC::Encode(r);
            {
                std::lock_guard lk(s.mu);
                s.replyCode = text;
            }
            Publish(text, true, "PULSAR_ReplyCode.txt",
                    s.cfg.clipboard ? "Co-op: can't reach the host yet. Reply code copied: send it to the host (F2 there)."
                                    : "Co-op: can't reach the host yet. Send PULSAR_ReplyCode.txt to the host.");
        }
    }

    void OnConnected(ENetHost* a_host, ENetPeer* a_winner)
    {
        if (s.role != Role::kClient) return;
        s.connected = true;
        for (std::size_t i = 0; i < a_host->peerCount; ++i) {
            ENetPeer* p = &a_host->peers[i];
            if (p != a_winner && p->state != ENET_PEER_STATE_DISCONNECTED) enet_peer_reset(p);
        }
        const char* via = "a LAN address";
        for (const auto& t : s.targets) {
            if (t.ipv4 == a_winner->address.host && t.port == a_winner->address.port) {
                via = t.via == JC::Via::kMapped ? "the port the host's router opened" :
                      t.via == JC::Via::kPublic ? "the host's public address (hole punching)" : "a LAN address";
                break;
            }
        }
        PLOG_DIAG("[PULSAR] co-op: connected directly to the host through {} ({})"sv, via,
                  ToString(a_winner->address.host, a_winner->address.port));
    }

    bool Joining() { return s.role == Role::kClient && !s.connected; }

    int ENET_CALLBACK Intercept(ENetHost* a_host, ENetEvent* /*a_event*/)
    {
        const auto* d = a_host->receivedData;
        const std::size_t n = a_host->receivedDataLength;
        if (!Stun::LooksLikeStun(d, n)) return 0;
        if (const auto m = Stun::ParseBindingResponse(d, n, s.tx)) {
            if (!s.stunHave && UsableIpv4(m->ipv4)) {
                s.stunHave = true;
                s.stunEp = { JC::Via::kPublic, m->ipv4, m->port };
                PLOG_DIAG("[PULSAR] co-op: public address of this PC: {}"sv, ToString(m->ipv4, m->port));
            }
        }
        return 1;
    }

    std::string HostCode()
    {
        std::lock_guard lk(s.mu);
        return s.hostCode;
    }

    std::string ReplyCode()
    {
        std::lock_guard lk(s.mu);
        return s.replyCode;
    }

    void Stop()
    {
        std::uint16_t ext = 0;
        {
            std::lock_guard lk(s.mu);
            if (s.upnpState.load() == 1) ext = s.upnpExtPort;
            s.replyTargets.clear();
        }
        s.role = Role::kNone;
        if (ext) {
            UpnpUnmap(ext);
            PLOG_INFO("[PULSAR] [Direct] UPnP mapping {} removed"sv, ext);
        }
        s.upnpState.store(2);
    }

}  // namespace Pulsar::Network::Direct
