#include "PCH.h"
#include "Coop/TakeoffVote.h"

#include "Config.h"
#include "Coop/TakeoffVotePolicy.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "SpaceSync/SpaceSync.h"

#include <RE/B/BSFixedString.h>

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <string>
#include <mutex>

using namespace std::string_view_literals;

namespace Pulsar::Coop::TakeoffVote {
namespace {

    enum Op : std::uint8_t { kRequest = 1, kReady = 2, kGo = 3, kCancelled = 4 };

    constexpr int kPartnerSlot = 1;   // one partner today (single m_peer)

    std::mutex        s_mutex;
    TakeoffVotePolicy s_pilot;                  // guarded by s_mutex
    std::uint32_t     s_partnerRequest = 0;     // partner: open request id
    std::uint64_t     s_partnerRequestMs = 0;
    bool              s_partnerAnswered = false;
    std::uint32_t     s_partnerGo = 0;          // partner: go received, board on the game thread
    // Pilot: "go" to send. OnPacketReceived runs inside the network thread's
    // receive, which holds NetworkHost's ENet mutex; Send takes it again
    // (run 20261028-205624: the host exited the moment "ready" arrived).
    std::uint32_t     s_sendGo = 0;
    std::atomic<bool> s_answerReady{ false };   // bot / hotkey -> game thread
    bool              s_keyWasDown = false;

    void Send(Op a_op, std::uint32_t a_id)
    {
        using namespace Pulsar::Network;
        std::uint8_t buf[1 + sizeof(TakeoffVoteData)];
        buf[0] = static_cast<std::uint8_t>(PKT_TAKEOFF_VOTE);
        TakeoffVoteData d{ a_op, a_id };
        std::memcpy(buf + 1, &d, sizeof(d));
        Pulsar::Network::GetSingleton().Send(buf, sizeof(buf), /*reliable=*/true);
    }

    // On-screen messages. Not through the Papyrus VM: a DispatchStaticCall of
    // Debug.Notification faulted inside the engine's dispatch on the game thread
    // (run 20261028-214018: AV at Starfield.exe+0x22CB6C1 under +0x285C759,
    // called from here; the game ended 3 s later). Nor CommonLibSF upstream's
    // DebugNotification ID 138728: on 1.16.244 it resolves to RVA 0x2878C50,
    // a copy routine that dereferences its 2nd argument (run 20261028-214810).
    //
    // Instead the Debug.Notification native itself, called directly. Found by
    // its registration: the code at RVA 0x2005495 passes "Notification"
    // (0x4D23F30) and the native 0x2003A60 to the NativeFunction constructor.
    // The native only reads its 4th argument (the message BSFixedString),
    // copies it into a task and queues that on the engine's main-thread task
    // queue; it never touches the VM, stack id or self arguments. 1.16.244 only:
    // the prologue is checked before the call. Queued here, shown from Tick,
    // so it never runs inside an event sink.
    std::mutex              s_msgMutex;
    std::deque<std::string> s_messages;

    void Notify(const char* a_msg)
    {
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] message: {}"sv, a_msg);
        if (!Pulsar::GetConfig().coopVoteMessages) return;
        std::lock_guard lk(s_msgMutex);
        if (s_messages.size() < 8) s_messages.emplace_back(a_msg);
    }

    using NotificationNative_t = void (*)(void* a_vm, std::uint32_t a_stackID, void* a_self,
                                          const RE::BSFixedString* a_msg);
    constexpr std::uintptr_t kRVA_NotificationNative = 0x2003A60;
    constexpr std::uint8_t   kNotificationPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48,
                                                         0x83, 0xEC, 0x60, 0x49, 0x8B, 0x19 };

    NotificationNative_t ResolveNotificationNative()
    {
        const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
        auto* fn = reinterpret_cast<const std::uint8_t*>(base + kRVA_NotificationNative);
        const bool match = std::memcmp(fn, kNotificationPrologue, sizeof(kNotificationPrologue)) == 0;
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] Debug.Notification native at RVA 0x{:X}: {}"sv,
                  kRVA_NotificationNative, match ? "prologue matches" : "PROLOGUE MISMATCH - messages off");
        return match ? reinterpret_cast<NotificationNative_t>(base + kRVA_NotificationNative) : nullptr;
    }

    bool SafeShow(NotificationNative_t a_fn, const RE::BSFixedString* a_msg) noexcept
    {
        __try {
            a_fn(nullptr, 0, nullptr, a_msg);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void ShowQueuedMessages()
    {
        std::deque<std::string> out;
        {
            std::lock_guard lk(s_msgMutex);
            out.swap(s_messages);
        }
        if (out.empty()) return;
        static const NotificationNative_t s_fn = ResolveNotificationNative();
        if (!s_fn) return;
        for (const auto& m : out) {
            const RE::BSFixedString msg(m.c_str());
            const bool ok = SafeShow(s_fn, &msg);
            PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] shown on screen: {} ({})"sv, m, ok ? "ok" : "FAULTED");
        }
    }

    const char* KeyName()
    {
        static char buf[16];
        const int vk = Pulsar::GetConfig().hotkeyTakeoffReady;
        if (vk >= 0x70 && vk <= 0x87) std::snprintf(buf, sizeof(buf), "F%d", vk - 0x6F);
        else std::snprintf(buf, sizeof(buf), "key 0x%02X", vk);
        return buf;
    }

}  // namespace

bool HoldLaunch()
{
    auto& net = Pulsar::Network::GetSingleton();
    if (!Pulsar::GetConfig().coopTakeoffVote || !net.IsConnected()) return false;
    TakeoffVotePolicy::Request r;
    std::uint32_t id;
    {
        std::lock_guard lk(s_mutex);
        r = s_pilot.LaunchRequested(GetTickCount64(), /*crew=*/1);
        id = s_pilot.Id();
    }
    if (r == TakeoffVotePolicy::Request::kPass) return false;
    if (r == TakeoffVotePolicy::Request::kHoldAndAsk) {
        Send(kRequest, id);
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] launch held, vote {} asks the crew (0/1 ready)"sv, id);
        Notify("Takeoff: waiting for your crew to be ready");
    } else {
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] launch held, vote {} still open"sv, id);
    }
    return true;
}

void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_length)
{
    using namespace Pulsar::Network;
    if (!a_data || a_length < 1 + sizeof(TakeoffVoteData)) return;
    TakeoffVoteData d{};
    std::memcpy(&d, a_data + 1, sizeof(d));
    std::lock_guard lk(s_mutex);
    switch (d.op) {
    case kRequest:
        s_partnerRequest = d.voteId;
        s_partnerRequestMs = GetTickCount64();
        s_partnerAnswered = false;
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] partner wants to take off (vote {})"sv, d.voteId);
        break;
    case kReady:
        if (s_pilot.Ready(d.voteId, kPartnerSlot, GetTickCount64()) == TakeoffVotePolicy::Answer::kSendGo) {
            s_sendGo = d.voteId;   // sent from Tick: Send here would re-lock the ENet mutex
            PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] crew ready for vote {} - boarding, launch in {} ms"sv,
                      d.voteId, TakeoffVotePolicy::kBoardMs);
        } else {
            PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] ready for vote {} ignored (not open)"sv, d.voteId);
        }
        break;
    case kGo:
        if (d.voteId == s_partnerRequest) s_partnerGo = d.voteId;
        break;
    case kCancelled:
        if (d.voteId == s_partnerRequest) {
            s_partnerRequest = 0;
            PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] partner's takeoff vote {} cancelled"sv, d.voteId);
        }
        break;
    default:
        break;
    }
}

void Tick()
{
    auto& net = Pulsar::Network::GetSingleton();
    const std::uint64_t now = GetTickCount64();
    ShowQueuedMessages();

    // Pilot side.
    TakeoffVotePolicy::Step step;
    std::uint32_t id, sendGo;
    {
        std::lock_guard lk(s_mutex);
        if (!net.IsConnected() && s_pilot.Open()) s_pilot.Cancel();
        step = s_pilot.Tick(now);
        id = s_pilot.Id();
        sendGo = s_sendGo;
        s_sendGo = 0;
    }
    if (sendGo) Send(kGo, sendGo);
    if (step == TakeoffVotePolicy::Step::kTimedOut) {
        Send(kCancelled, id);
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] vote {} timed out - takeoff cancelled"sv, id);
        Notify("Takeoff cancelled: your crew was not ready");
    } else if (step == TakeoffVotePolicy::Step::kLaunch) {
        const bool ok = Pulsar::SpaceSync::ReleaseHeldLaunch();
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] vote {} passed - launching (fired={})"sv, id, ok ? 1 : 0);
    }

    // Partner side.
    if (!net.IsConnected()) return;
    std::uint32_t request, go;
    bool answered;
    {
        std::lock_guard lk(s_mutex);
        if (s_partnerRequest && now - s_partnerRequestMs > TakeoffVotePolicy::kTimeoutMs + 5000)
            s_partnerRequest = 0;
        request = s_partnerRequest;
        answered = s_partnerAnswered;
        go = s_partnerGo;
        s_partnerGo = 0;
    }
    static std::uint32_t s_shown = 0;
    if (request && request != s_shown) {
        s_shown = request;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "Your partner wants to take off. Press %s when ready.", KeyName());
        Notify(msg);
    }
    const bool down = (GetAsyncKeyState(Pulsar::GetConfig().hotkeyTakeoffReady) & 0x8000) != 0;
    const bool pressed = down && !s_keyWasDown;
    s_keyWasDown = down;
    if (request && !answered && (pressed || s_answerReady.exchange(false))) {
        {
            std::lock_guard lk(s_mutex);
            s_partnerAnswered = true;
        }
        Send(kReady, request);
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] ready for vote {}"sv, request);
        Notify("Takeoff: you are ready");
    }
    if (go) {
        const char* what = Pulsar::SpaceSync::BoardPartnerShip();
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] vote {} passed - boarding: {}"sv, go, what);
        std::lock_guard lk(s_mutex);
        if (s_partnerRequest == go) s_partnerRequest = 0;
    }
}

void OnTakeoffMenuClosed()
{
    std::lock_guard lk(s_mutex);
    if (s_pilot.Open())
        PLOG_INFO("[PULSAR] [TAKEOFF-VOTE] takeoff menu closed while vote {} is open"sv, s_pilot.Id());
}

std::uint32_t PendingRequestForTest()
{
    std::lock_guard lk(s_mutex);
    return s_partnerAnswered ? 0 : s_partnerRequest;
}

void AnswerReadyForTest()
{
    s_answerReady.store(true);
}

const char* StateForTest()
{
    std::lock_guard lk(s_mutex);
    return s_pilot.Open() ? "open" : "idle";
}

}  // namespace Pulsar::Coop::TakeoffVote
