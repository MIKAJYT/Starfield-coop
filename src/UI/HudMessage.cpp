#include "PCH.h"
#include "UI/HudMessage.h"

#include <RE/B/BSFixedString.h>

#include <Windows.h>

#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

using namespace std::string_view_literals;

namespace Pulsar::UI::HudMessage {
namespace {

    // The Debug.Notification native, called directly.
    //
    // Not through the Papyrus VM: a C++ DispatchStaticCall of Debug.Notification
    // faulted inside the engine's dispatch on the game thread (run
    // 20261028-214018: AV at Starfield.exe+0x22CB6C1 under +0x285C759; the game
    // ended 3 s later). Not CommonLibSF upstream's DebugNotification ID 138728
    // either: on 1.16.244 it resolves to RVA 0x2878C50, a copy routine.
    //
    // The native (RVA 0x2003A60) was found by its registration: the code at RVA
    // 0x2005495 passes "Notification" (0x4D23F30) and it to the NativeFunction
    // constructor. It only reads its 4th argument, the message BSFixedString,
    // and fires the HUD notification event (source vtable 0x4C5E248, through
    // BSTEventSource::Notify 0x22C9300), the same event the game's own
    // notifications use. It must run on a worker thread, as the Papyrus VM
    // calls it: fired on the main thread the event is delivered but nothing is
    // drawn (runs 20261028-215638 to -221450); from a worker thread the text
    // appears (runs 20261028-230712, -231355). The prologue is checked before
    // the first call.
    using NotificationNative_t = void (*)(void* a_vm, std::uint32_t a_stackID, void* a_self,
                                          const RE::BSFixedString* a_msg);
    constexpr std::uintptr_t kRVA_NotificationNative = 0x2003A60;
    constexpr std::uint8_t   kNotificationPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48,
                                                         0x83, 0xEC, 0x60, 0x49, 0x8B, 0x19 };

    std::mutex              s_mutex;
    std::deque<std::string> s_queue;

    NotificationNative_t ResolveNotificationNative()
    {
        const auto base = REX::FModule::GetExecutingModule().GetBaseAddress();
        auto* fn = reinterpret_cast<const std::uint8_t*>(base + kRVA_NotificationNative);
        const bool match = std::memcmp(fn, kNotificationPrologue, sizeof(kNotificationPrologue)) == 0;
        PLOG_INFO("[PULSAR] HudMessage: Debug.Notification native at RVA 0x{:X}: {}"sv,
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

}  // namespace

void Show(const char* a_text)
{
    if (!a_text || !*a_text) return;
    std::lock_guard lk(s_mutex);
    if (s_queue.size() < 8) s_queue.emplace_back(a_text);
}

void Tick()
{
    std::deque<std::string> out;
    {
        std::lock_guard lk(s_mutex);
        out.swap(s_queue);
    }
    if (out.empty()) return;
    static const NotificationNative_t s_fn = ResolveNotificationNative();
    if (!s_fn) return;
    std::thread([texts = std::move(out)] {
        for (const auto& t : texts) {
            const RE::BSFixedString msg(t.c_str());
            const bool ok = SafeShow(s_fn, &msg);
            PLOG_INFO("[PULSAR] HudMessage: shown on screen: {} ({})"sv, t, ok ? "ok" : "FAULTED");
        }
    }).detach();
}

}  // namespace Pulsar::UI::HudMessage
