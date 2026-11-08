#include "PCH.h"
#include "SceneSync/SceneSync.h"

#include "Config.h"
#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "Papyrus/GameCalls.h"

#include <RE/B/BGSScene.h>
#include <RE/B/BSTEvent.h>
#include <format>
#include <RE/T/TESForm.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <mutex>
#include <string>

using namespace std::string_view_literals;

namespace Pulsar::SceneSync {
namespace {

    // The engine's TESSceneEvent has no struct in CommonLibSF. The probe reads
    // its first qwords raw and names every one that points at a form.
    struct SceneEventRaw
    {
        std::uint64_t q[6];
    };

    // TES event sources are static BSTEventSource<T> objects in the game's data
    // section (DialogSync uses three of them by RVA). Each starts with the vtable
    // of its BSTEventSource<T>, which the address library knows, so the source is
    // found by scanning the image for that vtable pointer. The scan is checked on
    // TESTopicInfoEvent, whose source DialogSync has at RVA 0x5978468.
    constexpr std::uintptr_t kRVA_TESTopicInfoEvent = 0x5978468;

    struct ImageSpan
    {
        std::uintptr_t base = 0;
        std::size_t    size = 0;
    };

    ImageSpan SafeImage() noexcept
    {
        ImageSpan s{};
        __try {
            s.base = REX::FModule::GetExecutingModule().GetBaseAddress();
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(s.base);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(s.base + dos->e_lfanew);
            s.size = nt->OptionalHeader.SizeOfImage;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s = {};
        }
        return s;
    }

    // RVAs of every 8-aligned qword in the image's writable sections equal to a_value.
    int SafeFindQword(const ImageSpan& a_img, std::uintptr_t a_value, std::uintptr_t* a_out, int a_max) noexcept
    {
        int n = 0;
        __try {
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_img.base);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_img.base + dos->e_lfanew);
            const auto* sec = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && n < a_max; ++i, ++sec) {
                if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
                const std::uintptr_t lo = (a_img.base + sec->VirtualAddress + 7) & ~std::uintptr_t{ 7 };
                const std::uintptr_t hi = a_img.base + sec->VirtualAddress + sec->Misc.VirtualSize;
                for (std::uintptr_t p = lo; p + 8 <= hi && n < a_max; p += 8) {
                    if (*reinterpret_cast<const std::uintptr_t*>(p) == a_value) a_out[n++] = p - a_img.base;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    // "FORMID[type]" when a_q points at a form, else empty.
    void SafeNameQword(std::uint64_t a_q, char* a_out, std::size_t a_cap) noexcept
    {
        a_out[0] = '\0';
        if (a_q < 0x10000 || (a_q >> 47) != 0) return;
        __try {
            auto* p = reinterpret_cast<const std::uint8_t*>(a_q);
            const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(p + 0x28);
            const std::uint8_t type = *(p + 0x2E);
            if (id == 0 || type == 0) return;
            auto* form = RE::TESForm::LookupByID(id);
            if (form != reinterpret_cast<const RE::TESForm*>(p)) return;
            const char* eid = form->GetFormEditorID();
            _snprintf_s(a_out, a_cap, _TRUNCATE, "%08X[t%02X]%s%s", id, type, (eid && eid[0]) ? " " : "", (eid && eid[0]) ? eid : "");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            a_out[0] = '\0';
        }
    }

    // "[type] EditorID" for a form ID, else empty.
    void SafeNameID(std::uint32_t a_id, char* a_out, std::size_t a_cap) noexcept
    {
        a_out[0] = '\0';
        __try {
            auto* form = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
            if (!form) return;
            const std::uint8_t type = *(reinterpret_cast<const std::uint8_t*>(form) + 0x2E);
            const char* eid = form->GetFormEditorID();
            _snprintf_s(a_out, a_cap, _TRUNCATE, "[t%02X] %s", type, (eid && eid[0]) ? eid : "-");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            a_out[0] = '\0';
        }
    }

    bool SafeCopyEvent(const SceneEventRaw& a_from, SceneEventRaw& a_to) noexcept
    {
        __try {
            a_to = a_from;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::atomic<int> s_logged{ 0 };
    constexpr int    kMaxLogged = 400;

    // Last events for the test bot: scene ID (first dword), type (second dword).
    struct Recent
    {
        std::uint32_t id;
        std::uint32_t type;
        std::uint64_t wallMs;
    };
    std::mutex          s_recentMutex;
    std::deque<Recent>  s_recent;

    std::uint64_t WallMs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    // Event type (second dword of the event): 0 started, 1 ended (run
    // 20261030-224126: SQ_CCT_Enviro_RakingScene logged 0, then 1 30 s later).
    constexpr std::uint32_t kStarted = 0;
    constexpr std::uint32_t kEnded = 1;

    // Host: tell every client. Network thread or game thread, Send is safe.
    void SendScene(std::uint32_t a_id, bool a_started)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsHost() || !net.IsConnected() || !Pulsar::GetConfig().coopSceneSync) return;
        std::uint8_t buf[6];
        buf[0] = Pulsar::Network::PKT_SCENE;
        std::memcpy(buf + 1, &a_id, 4);
        buf[5] = a_started ? 1 : 0;
        net.Send(buf, sizeof(buf), /*reliable=*/true);
    }

    // Client: scenes the host started or ended, applied on the game thread.
    struct Pending
    {
        std::uint32_t id;
        bool          started;
    };
    std::mutex           s_pendingMutex;
    std::vector<Pending> s_pending;

    // Start a_scene, its quest first when that isn't running (an ambient quest
    // can run on one machine only: run 20261030-224709, "cannot start scene
    // because its parent quest was not running"). VM callbacks run on a VM thread.
    void StartScene(RE::BGSScene* a_scene, std::uint32_t a_id)
    {
        auto* vm = Pulsar::GameCalls::VM();
        auto scene = Pulsar::GameCalls::ObjectFor(a_scene);
        if (!vm || !scene) {
            PLOG_WARN("[SCENE] host started {:08X}: no script object here"sv, a_id);
            return;
        }
        Pulsar::GameCalls::CallMethodForResult(vm, scene, "GetOwningQuest", {},
            [vm, scene, a_id](const RE::BSScript::Variable& a_quest) {
                if (!a_quest.is<RE::BSScript::Object>()) {
                    Pulsar::GameCalls::CallMethod(vm, scene, "Start", {});
                    PLOG_INFO("[SCENE] host started {:08X}: starting it (no owning quest)"sv, a_id);
                    return;
                }
                auto quest = RE::BSScript::get<RE::BSScript::Object>(a_quest);
                Pulsar::GameCalls::CallMethodForResult(vm, quest, "IsRunning", {},
                    [vm, scene, quest, a_id](const RE::BSScript::Variable& a_running) {
                        if (a_running.is<bool>() && RE::BSScript::get<bool>(a_running)) {
                            Pulsar::GameCalls::CallMethod(vm, scene, "Start", {});
                            PLOG_INFO("[SCENE] host started {:08X}: starting it"sv, a_id);
                            return;
                        }
                        // Quest.Start returns once the quest is up; start the scene then.
                        Pulsar::GameCalls::CallMethodForResult(vm, quest, "Start", {},
                            [vm, scene, a_id](const RE::BSScript::Variable& a_ok) {
                                const bool ok = a_ok.is<bool>() && RE::BSScript::get<bool>(a_ok);
                                Pulsar::GameCalls::CallMethod(vm, scene, "Start", {});
                                PLOG_INFO("[SCENE] host started {:08X}: its quest was not running, started it (ok={}), then the scene"sv,
                                          a_id, ok);
                            });
                    });
            });
    }

    struct SceneSink : public RE::BSTEventSink<SceneEventRaw>
    {
        RE::BSEventNotifyControl ProcessEvent(const SceneEventRaw& a_event, RE::BSTEventSource<SceneEventRaw>*) override
        {
            SceneEventRaw e{};
            if (!SafeCopyEvent(a_event, e)) return RE::BSEventNotifyControl::kContinue;
            {
                std::lock_guard lk(s_recentMutex);
                s_recent.push_back({ static_cast<std::uint32_t>(e.q[0]), static_cast<std::uint32_t>(e.q[0] >> 32), WallMs() });
                while (s_recent.size() > 32) s_recent.pop_front();
            }
            const auto id = static_cast<std::uint32_t>(e.q[0]);
            const auto type = static_cast<std::uint32_t>(e.q[0] >> 32);
            if (type == kStarted || type == kEnded) SendScene(id, type == kStarted);
            // The cap limits the log only: placed above the send, it ended the scene
            // sync for the rest of the session after 400 scene events (a city makes
            // them all the time).
            if (s_logged.fetch_add(1) >= kMaxLogged) return RE::BSEventNotifyControl::kContinue;
            {
                char nm[120];
                SafeNameID(id, nm, sizeof(nm));
                PLOG_INFO("[SCENE] {} event id={:08X} type={} {}"sv,
                          Pulsar::Network::GetSingleton().IsHost() ? "HOST" : "CLIENT", id, type, std::string(nm));
            }
            char line[700];
            int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s",
                                  Pulsar::Network::GetSingleton().IsHost() ? "HOST" : "CLIENT");
            for (int i = 0; i < 6 && len > 0; ++i) {
                char nm[120];
                SafeNameQword(e.q[i], nm, sizeof(nm));
                const int w = _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " q%d=%llX%s%s%s", i,
                                          static_cast<unsigned long long>(e.q[i]), nm[0] ? "(" : "", nm, nm[0] ? ")" : "");
                if (w < 0) break;
                len += w;
            }
            PLOG_INFO("[SCENE] {}"sv, std::string(line));
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    SceneSink s_sink;

    bool SafeRegister(RE::BSTEventSource<SceneEventRaw>* a_src) noexcept
    {
        __try {
            a_src->RegisterSink(&s_sink);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

}  // namespace

void Init()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    const ImageSpan img = SafeImage();
    if (!img.base) {
        PLOG_WARN("[SCENE] probe: no image base"sv);
        return;
    }
    std::uintptr_t vtTopic = 0, vtScene = 0;
    try {
        vtTopic = RE::VTABLE::BSTEventSource_TESTopicInfoEvent_[0].address();
        vtScene = RE::VTABLE::BSTEventSource_TESSceneEvent_[0].address();
    } catch (...) {
        PLOG_WARN("[SCENE] probe: vtable IDs not in the address library"sv);
        return;
    }

    std::uintptr_t hits[8];
    const int nTopic = SafeFindQword(img, vtTopic, hits, 8);
    bool topicOk = false;
    for (int i = 0; i < nTopic; ++i) topicOk |= (hits[i] == kRVA_TESTopicInfoEvent);
    PLOG_INFO("[SCENE] probe: TESTopicInfoEvent source vtable found at {} place(s), known RVA 0x{:X} {}"sv,
              nTopic, kRVA_TESTopicInfoEvent, topicOk ? "among them (scan works)" : "NOT among them");

    const int nScene = SafeFindQword(img, vtScene, hits, 8);
    for (int i = 0; i < nScene; ++i) PLOG_INFO("[SCENE] probe: TESSceneEvent source candidate RVA 0x{:X}"sv, hits[i]);
    if (!topicOk || nScene != 1) {
        PLOG_WARN("[SCENE] probe: not registering (scan check {}, {} scene candidate(s))"sv, topicOk ? "ok" : "failed", nScene);
        return;
    }
    auto* src = reinterpret_cast<RE::BSTEventSource<SceneEventRaw>*>(img.base + hits[0]);
    PLOG_INFO("[SCENE] probe: TESSceneEvent sink {}"sv, SafeRegister(src) ? "registered" : "RegisterSink threw");
}

void OnPacketReceived(const std::uint8_t* a_data, std::size_t a_len)
{
    if (a_len < 6 || a_data[0] != Pulsar::Network::PKT_SCENE) return;
    if (Pulsar::Network::GetSingleton().IsHost()) return;   // host-authoritative
    std::uint32_t id = 0;
    std::memcpy(&id, a_data + 1, 4);
    std::lock_guard lk(s_pendingMutex);
    s_pending.push_back({ id, a_data[5] != 0 });
}

void DrainPending()
{
    std::vector<Pending> work;
    {
        std::lock_guard lk(s_pendingMutex);
        work.swap(s_pending);
    }
    for (const auto& p : work) {
        if (!Pulsar::GetConfig().coopSceneSync) {
            PLOG_INFO("[SCENE] host {} {:08X}: CoopSceneSync = 0, ignored"sv, p.started ? "started" : "ended", p.id);
            continue;
        }
        auto* form = RE::TESForm::LookupByID(p.id);
        auto* scene = form ? form->As<RE::BGSScene>() : nullptr;
        if (!scene) {
            PLOG_WARN("[SCENE] host {} {:08X}: no such scene here"sv, p.started ? "started" : "ended", p.id);
            continue;
        }
        if (p.started) {
            StartScene(scene, p.id);
        } else if (auto obj = Pulsar::GameCalls::ObjectFor(scene)) {
            Pulsar::GameCalls::CallMethod(Pulsar::GameCalls::VM(), obj, "Stop", {});
            PLOG_INFO("[SCENE] host ended {:08X}: stopping it"sv, p.id);
        }
    }
}

bool StartSceneForTest(std::uint32_t a_id)
{
    auto* form = RE::TESForm::LookupByID(a_id);
    auto* scene = form ? form->As<RE::BGSScene>() : nullptr;
    if (!scene) return false;
    StartScene(scene, a_id);
    return true;
}

namespace {
    // The owning quest field of BGSScene is not in CommonLibSF. It is found on
    // SQ_CCT_Enviro_RakingScene (000D8783), whose quest SQ_CCT_Enviro_Raking
    // (000D8787) the Papyrus log names: the first qword in the scene that points
    // at that quest.
    constexpr std::uint32_t kCalScene = 0x000D8783;
    constexpr std::uint32_t kCalQuest = 0x000D8787;

    int SafeFindQuestOffset() noexcept
    {
        __try {
            auto* scene = RE::TESForm::LookupByID(kCalScene);
            auto* quest = RE::TESForm::LookupByID(kCalQuest);
            if (!scene || !quest) return -1;
            auto* b = reinterpret_cast<const std::uint8_t*>(scene);
            for (int off = 0x38; off < 0x200; off += 8) {
                if (*reinterpret_cast<RE::TESForm* const*>(b + off) == quest) return off;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return -1;
    }

    // The scene's quest is running (TESQuest +0x114 bit 0, see QuestSync) and
    // its editor ID contains a_hint. a_hint is lower case.
    bool SafeSceneMatches(RE::TESForm* a_scene, int a_off, const char* a_hint) noexcept
    {
        __try {
            if (!a_scene || *(reinterpret_cast<const std::uint8_t*>(a_scene) + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kSCEN)) return false;
            auto* quest = *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(a_scene) + a_off);
            if (!quest || *(quest + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kQUST)) return false;
            if (!(*(quest + 0x114) & 1)) return false;
            const char* eid = a_scene->GetFormEditorID();
            if (!eid) return false;
            char low[256];
            std::size_t i = 0;
            for (; eid[i] && i + 1 < sizeof(low); ++i) low[i] = static_cast<char>(::tolower(static_cast<unsigned char>(eid[i])));
            low[i] = '\0';
            return std::strstr(low, a_hint) != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

std::vector<std::uint32_t> RunningScenesForTest(const char* a_hint, std::size_t a_max, std::string& a_info)
{
    std::vector<std::uint32_t> out;
    const int off = SafeFindQuestOffset();
    if (off < 0) {
        a_info = "owning-quest field not found on 000D8783";
        return out;
    }
    std::string hint = a_hint ? a_hint : "";
    for (auto& c : hint) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    // TESDataHandler's scene array came back empty (run 20261030-230707, its
    // layout differs on 1.16.244), so walk Starfield.esm's form IDs instead:
    // one hash lookup each, well under a second, once per test.
    std::size_t total = 0;
    for (std::uint32_t id = 0x800; id < 0x00400000 && out.size() < a_max; ++id) {
        auto* f = RE::TESForm::LookupByID(id);
        if (!f || *(reinterpret_cast<const std::uint8_t*>(f) + 0x2E) != static_cast<std::uint8_t>(RE::FormType::kSCEN)) continue;
        ++total;
        if (SafeSceneMatches(f, off, hint.c_str())) out.push_back(id);
    }
    a_info = "quest field +0x" + std::format("{:X}", off) + ", " + std::to_string(out.size()) + " of " +
             std::to_string(total) + " scenes seen";
    return out;
}

std::uint64_t LastStartWallMsForTest(std::uint32_t a_id)
{
    std::lock_guard lk(s_recentMutex);
    std::uint64_t last = 0;
    for (const auto& r : s_recent) {
        if (r.id == a_id && r.type == kStarted) last = r.wallMs;
    }
    return last;
}

std::string RecentEventsForTest()
{
    std::lock_guard lk(s_recentMutex);
    std::string s;
    for (const auto& r : s_recent) {
        char c[48];
        std::snprintf(c, sizeof(c), "%s%08X:%u:%llu", s.empty() ? "" : ",", r.id, r.type,
                      static_cast<unsigned long long>(r.wallMs));
        s += c;
    }
    return s;
}

}  // namespace Pulsar::SceneSync
