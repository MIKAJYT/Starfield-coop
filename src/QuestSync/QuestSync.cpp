#include "PCH.h"
#include "QuestSync/QuestSync.h"

#include "Network/NetworkHost.h"
#include "Network/PacketTypes.h"
#include "Config.h"
#include "EventTrace/EventTrace.h"
#include "Papyrus/GameCalls.h"
#include "QuestSync/StageJournal.h"
#include "TransitSync/TransitSync.h"

#include <REL/Relocation.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESQuest.h>

#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::QuestSync {
namespace {

    // L.8 Phase 1 - native TESQuest stage-set worker. Verified 2026-05-09 via
    // Ghidra wrapper-chain decompile (FUN_1420CF3A0 Papyrus → FUN_1420D54D0
    // inner → FUN_140D0E1F0 core). Clean signature, no Papyrus VM dependency.
    // The worker:
    // Gates on TESQuest+0x114 bit 0 (running flag)
    // Validates stage exists in the per-quest stage hash table @ +0x1A0
    // Calls subordinate stage-script worker FUN_140D2B290
    // Updates highestSetStageID @ +0x328 if newStage > current
    // Calls vt[0x17] with 0x80000000 (HUD recalc)
    // Calls FUN_140CF2E60 which Notifies TESQuestStageEvent on the global
    //     BSTEventSource @ RVA 0x5978238
    // Returns true if a stage script was found+executed, false otherwise.
    using SetStageWorker_t = bool (*)(RE::TESQuest*, std::uint16_t);
    static constexpr std::uintptr_t kRVA_SetStageWorker = 0xD0DAC0;

    static SetStageWorker_t g_origSetStage = nullptr;

    // L.8 Phase 2 - TESQuest::Start engine core. RE'd 2026-05-09 (Ghidra
    // sub-agent): only call site of the StartStopEvent Notify wrapper
    // FUN_140B984F0 in the entire binary. Sets +0x114 |= 0x80 (priming),
    // expands aliases/objectives, clears bit 7, sets *outOk=1, fires
    // BSTEventSource<TESQuestStartStopEvent>::Notify(formID, started=1).
    // Signature: param_1 = QuestStartCtx* (caller frame, not portable),
    // param_2 = TESQuest** (deref to get the quest), param_3 = byte* outOk.
    // We hook only for TX broadcast - RX does NOT replay this because
    // QuestStartCtx is caller-frame state we can't reconstruct on the peer.
    // Phase 1 stage sync triggers quest-start naturally on the peer when
    // the initial-stage scripts run (Bethesda's standard pattern).
    using StartWorker_t = void (*)(void* ctx, RE::TESQuest** questPtr, char* outOk);
    static constexpr std::uintptr_t kRVA_StartWorker = 0xEEF850;
    static StartWorker_t g_origStart = nullptr;

    // L.8 Phase 2 - TESQuest::Stop engine core. RE'd 2026-05-09 (Ghidra
    // sub-agent): clean (TESQuest*, char) signature. Walks aliases,
    // clears +0x114 bit 0 (running flag), calls vt[0xb8](this, 2), fires
    // stage event via FUN_140CF2DF0 Notify, then if param_2 != 0 calls
    // FUN_140B984F0(formID, started=0, ?) to fire StartStopEvent.
    // Hook for TX broadcast + RX replay (signature is fully portable).
    using StopWorker_t = void (*)(RE::TESQuest* quest, char param_2);
    static constexpr std::uintptr_t kRVA_StopWorker = 0xCF9970;
    static StopWorker_t g_origStop = nullptr;

    // Echo-suppression: when applying a remote stage update we still call the
    // worker, which still fires TESQuestStageEvent locally (and thus our hook
    // re-fires recursively because the hook IS the worker). The flag tells
    // the broadcast path to skip emission so we don't loop {RX → worker →
    // hook → broadcast → peer → RX → ...}. Per-thread atomic - Network and
    // game-thread can both hit the worker.
    static thread_local bool tls_applyingRemoteStage = false;
    static thread_local bool tls_applyingRemoteStop  = false;

    // Dedupe identical Start/Stop broadcasts. Quest start fires from
    // multiple paths (Papyrus, StoryTeller drain, ScriptEvent activation)
    // so the worker can run twice in rapid succession without state
    // change. Map keyed by formID; value 0=stopped, 1=started. Reset()
    // clears so a reconnect re-broadcasts cleanly.
    static std::mutex                                s_lastStartStopMutex;
    static std::unordered_map<std::uint32_t, std::uint8_t> s_lastStartStop;

    // Drop duplicate stage broadcasts. Useful because some quests call
    // SetCurrentStageID on the SAME stage (the engine path is idempotent -
    // worker returns true if scripts ran, and they may run multiple times
    // per stage e.g. across save/load). Keyed by quest formID, valued by
    // last-broadcast stageID. Reset() clears so reconnects don't suppress.
    static std::mutex                                s_lastSeenMutex;
    static std::unordered_map<std::uint32_t, std::uint16_t> s_lastSeenStage;

    // 0.5.0 missed-stage replay: every local stage set, and when the peer
    // last dropped (0 = no gap to replay). NOT cleared by Reset().
    static std::mutex                 s_journalMutex;
    static StageJournal               s_journal;
    static std::atomic<std::uint64_t> s_gapStartMs{ 0 };

    // Raw +0x28 read for TESForm.formID - verified universal offset on
    // 1.16.236 (memory/reference_formid_offset.md). Avoids vt-call on a
    // potentially mid-mutation TESQuest from inside the hook body.
    static std::uint32_t RawFormID(const void* form) noexcept
    {
        if (!form) return 0;
        return *reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(form) + 0x28);
    }

    // Forward declarations - bodies live near the bottom of this namespace
    // (kept down there because they wrap raw worker calls in __try, and
    // C2712 forbids __try anywhere upstream of std::lock_guard scopes).
    static bool          SafeCallOrigSetStage(RE::TESQuest* quest, std::uint16_t stage) noexcept;
    static std::uint32_t SafeRawFormID(const void* form) noexcept;
    static void          SafeCallOrigStart(void* ctx, RE::TESQuest** questPtr, char* outOk) noexcept;
    static void          SafeCallOrigStop(RE::TESQuest* quest, char param_2) noexcept;
    static RE::TESQuest* SafeDerefQuestPtr(RE::TESQuest** pp) noexcept;
    static bool          SafeIsRunning(const RE::TESQuest* quest) noexcept;
    static void          RecordStart(std::uint32_t formID);
    static bool          IsQuietStop(std::uint32_t formID);

    static void BroadcastStage(std::uint32_t questFormID, std::uint16_t stageID)
    {
        // Skip if peer not connected - saves serialization cost and the
        // s_lastSeen book-keeping is irrelevant when nobody listens.
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        // Dedupe identical (formID, stageID) tuple. Worker can be called
        // multiple times per stage during save-load + script re-fires.
        {
            std::lock_guard g(s_lastSeenMutex);
            auto it = s_lastSeenStage.find(questFormID);
            if (it != s_lastSeenStage.end() && it->second == stageID) {
                return;
            }
            s_lastSeenStage[questFormID] = stageID;
        }

        std::uint8_t buf[1 + sizeof(Pulsar::Network::QuestStageData)]{};
        buf[0] = Pulsar::Network::PKT_QUEST_STAGE;
        Pulsar::Network::QuestStageData payload{ questFormID, stageID };
        std::memcpy(buf + 1, &payload, sizeof(payload));

        net.Send(buf, sizeof(buf), /*reliable=*/true);

        PLOG_INFO(
            "[PULSAR] QuestSync TX: quest=0x{:08X} stage={}"sv,
            questFormID, stageID);
    }

    // Hook body - runs in place of the engine's worker. Calls original
    // first so the engine state advances exactly as it would have without
    // the hook, then broadcasts if (a) original succeeded and (b) we're
    // not currently applying a remote update.
    static bool __fastcall HookedSetStage(RE::TESQuest* quest, std::uint16_t stage)
    {
        bool result = SafeCallOrigSetStage(quest, stage);

        if (!result) return result;

        // EventTrace - every local quest-stage change, named, even when solo or
        // when this is a remote-applied set. The single richest signal for
        // quest-progress sync design.
        if (quest) {
            std::uint32_t tfid = SafeRawFormID(quest);
            Pulsar::EventTrace::Trace("QUEST", "stage SET quest=%s stage=%u%s",
                Pulsar::EventTrace::NameOf(tfid).c_str(), stage,
                tls_applyingRemoteStage ? " (remote)" : "");
        }

        if (tls_applyingRemoteStage) return result;
        if (!quest) return result;

        std::uint32_t formID = SafeRawFormID(quest);
        if (!formID) return result;

        {
            std::lock_guard g(s_journalMutex);
            s_journal.Record(formID, stage, GetTickCount64());
        }
        BroadcastStage(formID, stage);
        return result;
    }

    // Phase 2 - broadcast helper for Start/Stop. Dedupes via s_lastStartStop
    // map so the same transition isn't broadcast twice.
    static void BroadcastStartStop(std::uint8_t pktType, std::uint32_t formID,
                                   std::uint16_t flag)
    {
        auto& net = Pulsar::Network::GetSingleton();
        if (!net.IsConnected()) return;

        const std::uint8_t startedVal =
            (pktType == Pulsar::Network::PKT_QUEST_START) ? 1 : 0;
        {
            std::lock_guard g(s_lastStartStopMutex);
            auto it = s_lastStartStop.find(formID);
            if (it != s_lastStartStop.end() && it->second == startedVal) {
                return;
            }
            s_lastStartStop[formID] = startedVal;
        }

        std::uint8_t buf[1 + sizeof(Pulsar::Network::QuestStartStopData)]{};
        buf[0] = pktType;
        Pulsar::Network::QuestStartStopData payload{ formID, flag };
        std::memcpy(buf + 1, &payload, sizeof(payload));
        net.Send(buf, sizeof(buf), /*reliable=*/true);

        PLOG_INFO(
            "[PULSAR] QuestSync TX: quest=0x{:08X} {} flag={}"sv,
            formID,
            startedVal ? "START" : "STOP",
            flag);
    }

    // Phase 2 - TESQuest::Start hook. Calls orig, then broadcasts on TX
    // path. Note: NO echo-suppression flag is used here because we never
    // replay this function on RX (caller-frame ctx is non-portable).
    static void __fastcall HookedStart(void* ctx, RE::TESQuest** questPtr, char* outOk)
    {
        SafeCallOrigStart(ctx, questPtr, outOk);

        RE::TESQuest* quest = SafeDerefQuestPtr(questPtr);
        if (!quest) return;

        std::uint32_t formID = SafeRawFormID(quest);
        if (!formID) return;

        Pulsar::EventTrace::Trace("QUEST", "START quest=%s",
            Pulsar::EventTrace::NameOf(formID).c_str());

        if (SafeIsRunning(quest)) RecordStart(formID);
        BroadcastStartStop(Pulsar::Network::PKT_QUEST_START, formID, 0);
    }

    // Phase 2 - TESQuest::Stop hook. Calls orig, then broadcasts unless
    // we're currently applying a remote stop (echo suppression). param_2
    // is forwarded as the payload flag so the receiver can distinguish
    // hard-stop (1+) from quiet cleanup (0).
    static void __fastcall HookedStop(RE::TESQuest* quest, char param_2)
    {
        SafeCallOrigStop(quest, param_2);

        if (tls_applyingRemoteStop) return;
        if (!quest) return;

        std::uint32_t formID = SafeRawFormID(quest);
        if (!formID) return;
        if (IsQuietStop(formID)) return;

        Pulsar::EventTrace::Trace("QUEST", "STOP quest=%s flag=%d",
            Pulsar::EventTrace::NameOf(formID).c_str(),
            static_cast<int>(static_cast<std::uint8_t>(param_2)));

        BroadcastStartStop(Pulsar::Network::PKT_QUEST_STOP, formID,
                           static_cast<std::uint16_t>(static_cast<std::uint8_t>(param_2)));
    }

    static std::atomic<bool> s_initDone{ false };

    // SEH-only helper. C2712 forbids __try in functions that contain
    // objects with non-trivial destructors (lock_guard, etc.), so the
    // raw worker call lives here and the public API holds the locks.
    static bool SafeCallOrigSetStage(RE::TESQuest* quest, std::uint16_t stage) noexcept
    {
        if (!g_origSetStage) return false;
        __try {
            return g_origSetStage(quest, stage);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static std::uint32_t SafeRawFormID(const void* form) noexcept
    {
        __try { return RawFormID(form); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static void SafeCallOrigStart(void* ctx, RE::TESQuest** questPtr, char* outOk) noexcept
    {
        if (!g_origStart) return;
        __try { g_origStart(ctx, questPtr, outOk); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void SafeCallOrigStop(RE::TESQuest* quest, char param_2) noexcept
    {
        if (!g_origStop) return;
        __try { g_origStop(quest, param_2); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static RE::TESQuest* SafeDerefQuestPtr(RE::TESQuest** pp) noexcept
    {
        if (!pp) return nullptr;
        __try { return *pp; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // TESQuest::startEventData (+0x330, CommonLibSF): set for quests the story
    // manager starts (mission boards, ambient events). Papyrus Quest.Start
    // refuses those ("event scoped quest outside of story manager", run
    // 20261030-225533), and their aliases (the mission's targets) are picked at
    // random per machine, so a client gets them from the host's save instead.
    static bool SafeIsStoryQuest(const RE::TESQuest* quest) noexcept
    {
        if (!quest) return false;
        __try { return *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(quest) + 0x330) != nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Test bot: quests whose Stop must not be sent to the peer (the client bot
    // stops a mission locally to play "the host has a mission the client lacks").
    static std::mutex                        s_quietStopMutex;
    static std::unordered_map<std::uint32_t, bool> s_quietStop;

    // TESQuest +0x114 bit 0: running (the stage worker skips a quest without it).
    static bool SafeIsRunning(const RE::TESQuest* quest) noexcept
    {
        if (!quest) return false;
        __try { return (*(reinterpret_cast<const std::uint8_t*>(quest) + 0x114) & 1) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static std::uint64_t WallMs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    // Quest start sync: a quest the peer started, or a stage for one, that is
    // not running here. The stage worker skips quests that aren't running, so
    // without a start every stage the host sets for a new quest is lost here.
    // Applied on the game thread (DrainPending): Papyrus Quest.Start, then the
    // stage. Quests only the story manager may start stay as they are.
    constexpr std::uint16_t kNoStage = 0xFFFF;
    struct PendingStart
    {
        std::uint32_t id;
        std::uint16_t stage;   // kNoStage: start only
        bool          tried;   // Quest.Start already called
    };
    static std::mutex                s_pendingStartMutex;
    static std::vector<PendingStart> s_pendingStarts;

    static void QueueStart(std::uint32_t id, std::uint16_t stage, bool tried)
    {
        std::lock_guard g(s_pendingStartMutex);
        s_pendingStarts.push_back({ id, stage, tried });
    }

    // Test bot: quests that started here (the Start hook), with wall time.
    static std::mutex                                        s_recentStartMutex;
    static std::deque<std::pair<std::uint32_t, std::uint64_t>> s_recentStarts;

    static bool IsQuietStop(std::uint32_t formID)
    {
        std::lock_guard g(s_quietStopMutex);
        return s_quietStop.count(formID) != 0;
    }

    static void RecordStart(std::uint32_t formID)
    {
        std::lock_guard g(s_recentStartMutex);
        s_recentStarts.emplace_back(formID, WallMs());
        while (s_recentStarts.size() > 32) s_recentStarts.pop_front();
    }

}  // anonymous namespace

void Init()
{
    if (s_initDone.exchange(true)) return;

    // Resolve via raw RVA - no stable REL::ID for FUN_140D0E1F0 known.
    // Pattern matches LootSync M.2 V3 (AddObjInner @ raw 0xB4CBC0).
    std::uintptr_t moduleBase =
        REX::FModule::GetExecutingModule().GetBaseAddress();
    void* fn = reinterpret_cast<void*>(moduleBase + kRVA_SetStageWorker);

    MH_STATUS st = MH_CreateHook(
        fn,
        reinterpret_cast<LPVOID>(&HookedSetStage),
        reinterpret_cast<LPVOID*>(&g_origSetStage));
    if (st != MH_OK) {
        PLOG_WARN(
            "[PULSAR] QuestSync: MH_CreateHook(SetCurrentStageID) failed: {}"sv,
            static_cast<int>(st));
        return;
    }
    st = MH_EnableHook(fn);
    if (st != MH_OK) {
        PLOG_WARN(
            "[PULSAR] QuestSync: MH_EnableHook(SetCurrentStageID) failed: {}"sv,
            static_cast<int>(st));
        return;
    }

    PLOG_INFO(
        "[PULSAR] QuestSync: TESQuest::SetCurrentStageID hook installed at "
        "0x{:x} (RVA 0x{:X})"sv,
        reinterpret_cast<std::uintptr_t>(fn),
        kRVA_SetStageWorker);

    // Phase 2 - Start hook. RVA 0xEEF850.
    {
        void* startFn = reinterpret_cast<void*>(moduleBase + kRVA_StartWorker);
        MH_STATUS sst = MH_CreateHook(
            startFn,
            reinterpret_cast<LPVOID>(&HookedStart),
            reinterpret_cast<LPVOID*>(&g_origStart));
        if (sst != MH_OK) {
            PLOG_WARN(
                "[PULSAR] QuestSync: MH_CreateHook(Start) failed: {}"sv,
                static_cast<int>(sst));
        } else {
            sst = MH_EnableHook(startFn);
            if (sst != MH_OK) {
                PLOG_WARN(
                    "[PULSAR] QuestSync: MH_EnableHook(Start) failed: {}"sv,
                    static_cast<int>(sst));
            } else {
                PLOG_INFO(
                    "[PULSAR] QuestSync: TESQuest::Start hook installed at "
                    "0x{:x} (RVA 0x{:X})"sv,
                    reinterpret_cast<std::uintptr_t>(startFn),
                    kRVA_StartWorker);
            }
        }
    }

    // Phase 2 - Stop hook. RVA 0xCF9970.
    {
        void* stopFn = reinterpret_cast<void*>(moduleBase + kRVA_StopWorker);
        MH_STATUS sst = MH_CreateHook(
            stopFn,
            reinterpret_cast<LPVOID>(&HookedStop),
            reinterpret_cast<LPVOID*>(&g_origStop));
        if (sst != MH_OK) {
            PLOG_WARN(
                "[PULSAR] QuestSync: MH_CreateHook(Stop) failed: {}"sv,
                static_cast<int>(sst));
        } else {
            sst = MH_EnableHook(stopFn);
            if (sst != MH_OK) {
                PLOG_WARN(
                    "[PULSAR] QuestSync: MH_EnableHook(Stop) failed: {}"sv,
                    static_cast<int>(sst));
            } else {
                PLOG_INFO(
                    "[PULSAR] QuestSync: TESQuest::Stop hook installed at "
                    "0x{:x} (RVA 0x{:X})"sv,
                    reinterpret_cast<std::uintptr_t>(stopFn),
                    kRVA_StopWorker);
            }
        }
    }
}

void Shutdown()
{
    // No-op: MinHook persists hooks until plugin teardown. Engine event
    // sources live in static memory; we never tear ours down here.
}

void OnPacketReceived(const std::uint8_t* data, std::size_t length)
{
    using namespace Pulsar::Network;

    if (!data || length < 1) return;
    const std::uint8_t pktType = data[0];

    if (pktType == PKT_QUEST_STAGE || pktType == PKT_QUEST_STAGE_REPLAY) {
        if (length < 1 + sizeof(QuestStageData)) return;

        QuestStageData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        // A replayed stage the client already reached on its own during the gap
        // would re-run that stage's scripts (duplicate rewards): skip it.
        if (pktType == PKT_QUEST_STAGE_REPLAY) {
            const std::uint64_t gap = s_gapStartMs.load(std::memory_order_acquire);
            bool already = false;
            {
                std::lock_guard g(s_journalMutex);
                already = gap != 0 && s_journal.ContainsSince(d.questFormID, d.stageID, gap);
            }
            if (already) {
                PLOG_INFO("[PULSAR] QuestSync RX replay: quest=0x{:08X} stage={} already set here - skip"sv,
                          d.questFormID, d.stageID);
                return;
            }
        }

        PLOG_INFO(
            "[PULSAR] QuestSync RX: quest=0x{:08X} stage={}"sv,
            d.questFormID, d.stageID);

        if (!g_origSetStage) {
            PLOG_WARN(
                "[PULSAR] QuestSync RX: orig worker null (Init didn't run?)"sv);
            return;
        }

        // Resolve TESQuest by formID. LookupByID is safe to call from any
        // thread (read-only hashtable lookup); the apply itself touches
        // mutable engine state but the worker is internally guarded by
        // engine locks.
        auto* form = RE::TESForm::LookupByID(d.questFormID);
        if (!form) {
            PLOG_INFO(
                "[PULSAR] QuestSync RX: quest 0x{:08X} not loaded - drop"sv,
                d.questFormID);
            return;
        }
        // Reject non-quest forms (defensive: cross-machine FormID drift
        // could theoretically resolve to a different type).
        if (form->GetFormType() != RE::FormType::kQUST) {
            PLOG_WARN(
                "[PULSAR] QuestSync RX: 0x{:08X} resolved as type {} not QUST - drop"sv,
                d.questFormID, static_cast<int>(form->GetFormType()));
            return;
        }
        auto* quest = static_cast<RE::TESQuest*>(form);

        // The worker skips a quest that isn't running: start it first.
        if (!SafeIsRunning(quest) && Pulsar::GetConfig().coopQuestStartSync) {
            {
                std::lock_guard g(s_lastStartStopMutex);
                s_lastStartStop[d.questFormID] = 1;
            }
            QueueStart(d.questFormID, d.stageID, false);
            PLOG_INFO("[PULSAR] QuestSync RX: quest=0x{:08X} stage={} - quest not running here, starting it first"sv,
                      d.questFormID, d.stageID);
            return;
        }

        // Pre-update s_lastSeenStage so the hook's immediate post-call
        // broadcast path dedupes against this stage even if the
        // tls flag misses any nested re-entry path.
        {
            std::lock_guard g(s_lastSeenMutex);
            s_lastSeenStage[d.questFormID] = d.stageID;
        }

        tls_applyingRemoteStage = true;
        bool ok = SafeCallOrigSetStage(quest, d.stageID);
        tls_applyingRemoteStage = false;

        PLOG_INFO(
            "[PULSAR] QuestSync RX applied: quest=0x{:08X} stage={} ok={}"sv,
            d.questFormID, d.stageID, ok ? 1 : 0);
        return;
    }

    // PKT_QUEST_START - Phase 2. The engine Start core (FUN_140EEFEE0) takes
    // a caller-frame QuestStartCtx that we can't reconstruct on the peer, and
    // stage sync does NOT start a quest here: the stage worker skips quests
    // that aren't running. So a quest the peer started is started here through
    // Papyrus Quest.Start on the game thread (CoopQuestStartSync). Quests only
    // the story manager may start (mission boards, ambient events) refuse it.
    if (pktType == PKT_QUEST_START) {
        if (length < 1 + sizeof(QuestStartStopData)) return;
        QuestStartStopData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        // Pre-update last-seen so the local TX hook (if it later fires
        // for the same start triggered by stage cascade) dedupes.
        {
            std::lock_guard g(s_lastStartStopMutex);
            s_lastStartStop[d.questFormID] = 1;
        }

        auto* form = RE::TESForm::LookupByID(d.questFormID);
        auto* quest = (form && form->GetFormType() == RE::FormType::kQUST) ? static_cast<RE::TESQuest*>(form) : nullptr;
        const bool running = SafeIsRunning(quest);
        const bool apply = quest && !running && Pulsar::GetConfig().coopQuestStartSync;
        if (apply) QueueStart(d.questFormID, kNoStage, false);
        PLOG_INFO(
            "[PULSAR] QuestSync RX: quest=0x{:08X} START (peer started; {})"sv,
            d.questFormID,
            !quest ? "not loaded here" : running ? "already running here" : apply ? "starting it here" : "CoopQuestStartSync = 0, not started");
        return;
    }

    // PKT_QUEST_STOP - Phase 2. RX replays via TESQuest::Stop core
    // (FUN_140CFA0A0) with clean (TESQuest*, char) signature. Echo
    // suppression flag prevents the orig's StartStopEvent Notify from
    // re-broadcasting back to the peer.
    if (pktType == PKT_QUEST_STOP) {
        if (length < 1 + sizeof(QuestStartStopData)) return;
        QuestStartStopData d{};
        std::memcpy(&d, data + 1, sizeof(d));

        PLOG_INFO(
            "[PULSAR] QuestSync RX: quest=0x{:08X} STOP flag={}"sv,
            d.questFormID, d.stageOrFlag);

        if (!g_origStop) {
            PLOG_WARN("[PULSAR] QuestSync RX: orig Stop null"sv);
            return;
        }

        auto* form = RE::TESForm::LookupByID(d.questFormID);
        if (!form) {
            PLOG_INFO(
                "[PULSAR] QuestSync RX: stop quest 0x{:08X} not loaded - drop"sv,
                d.questFormID);
            return;
        }
        if (form->GetFormType() != RE::FormType::kQUST) {
            PLOG_WARN(
                "[PULSAR] QuestSync RX: 0x{:08X} resolved as type {} not QUST - drop"sv,
                d.questFormID, static_cast<int>(form->GetFormType()));
            return;
        }
        auto* quest = static_cast<RE::TESQuest*>(form);

        {
            std::lock_guard g(s_lastStartStopMutex);
            s_lastStartStop[d.questFormID] = 0;
        }

        const char param2 = static_cast<char>(d.stageOrFlag & 0xFF);
        tls_applyingRemoteStop = true;
        SafeCallOrigStop(quest, param2);
        tls_applyingRemoteStop = false;

        PLOG_INFO(
            "[PULSAR] QuestSync RX applied: quest=0x{:08X} STOP flag={}"sv,
            d.questFormID, static_cast<int>(param2));
        return;
    }
}

void NotePeerDisconnected()
{
    std::uint64_t expected = 0;
    s_gapStartMs.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_acq_rel);
}

void ForgetMissedStages()
{
    s_gapStartMs.store(0, std::memory_order_release);
}

void ReplayMissedStagesToPeer()
{
    using namespace Pulsar::Network;
    const std::uint64_t gap = s_gapStartMs.exchange(0, std::memory_order_acq_rel);
    if (gap == 0) return;
    std::vector<StageEntry> missed;
    {
        std::lock_guard g(s_journalMutex);
        missed = s_journal.Since(gap);
    }
    auto& net = Pulsar::Network::GetSingleton();
    for (const auto& e : missed) {
        std::uint8_t buf[1 + sizeof(QuestStageData)]{};
        buf[0] = PKT_QUEST_STAGE_REPLAY;
        QuestStageData payload{ e.quest, e.stage };
        std::memcpy(buf + 1, &payload, sizeof(payload));
        net.Send(buf, sizeof(buf), /*reliable=*/true);
    }
    PLOG_DIAG("[PULSAR] co-op: replayed {} quest stage(s) set while the partner was disconnected"sv,
              missed.size());
}

void Reset()
{
    {
        std::lock_guard g(s_lastSeenMutex);
        s_lastSeenStage.clear();
    }
    {
        std::lock_guard g(s_lastStartStopMutex);
        s_lastStartStop.clear();
    }
    tls_applyingRemoteStage = false;
    tls_applyingRemoteStop  = false;
    PLOG_INFO("[PULSAR] QuestSync: Reset (last-seen + start/stop caches cleared)"sv);
}

namespace {
    // Papyrus Quest.Start; the callback re-queues the entry so the stage is
    // applied on the game thread once the quest runs.
    void StartViaPapyrus(RE::TESQuest* quest, std::uint32_t id, std::uint16_t stage)
    {
        auto* vm = Pulsar::GameCalls::VM();
        auto obj = Pulsar::GameCalls::ObjectFor(quest);
        if (!vm || !obj) {
            PLOG_WARN("[PULSAR] QuestSync: quest=0x{:08X} has no script object here, not started"sv, id);
            return;
        }
        Pulsar::GameCalls::CallMethodForResult(vm, obj, "Start", {},
            [id, stage](const RE::BSScript::Variable& a_ok) {
                const bool ok = a_ok.is<bool>() && RE::BSScript::get<bool>(a_ok);
                PLOG_INFO("[PULSAR] QuestSync: started quest=0x{:08X} for the peer: ok={}"sv, id, ok);
                QueueStart(id, stage, true);
            });
    }
}

void DrainPending()
{
    std::vector<PendingStart> work;
    {
        std::lock_guard g(s_pendingStartMutex);
        work.swap(s_pendingStarts);
    }
    for (const auto& p : work) {
        auto* form = RE::TESForm::LookupByID(p.id);
        auto* quest = (form && form->GetFormType() == RE::FormType::kQUST) ? static_cast<RE::TESQuest*>(form) : nullptr;
        if (!quest) continue;
        if (SafeIsRunning(quest)) {
            if (p.stage == kNoStage) continue;
            {
                std::lock_guard g(s_lastSeenMutex);
                s_lastSeenStage[p.id] = p.stage;
            }
            tls_applyingRemoteStage = true;
            const bool ok = SafeCallOrigSetStage(quest, p.stage);
            tls_applyingRemoteStage = false;
            PLOG_INFO("[PULSAR] QuestSync RX applied after start: quest=0x{:08X} stage={} ok={}"sv, p.id, p.stage, ok ? 1 : 0);
            continue;
        }
        // A quest no script can start here (Quest.Start said no: a story-manager
        // quest, whose startEventData a stopped copy no longer has, run
        // 20261031-000906), or one known to be a story-manager quest.
        if (p.tried || SafeIsStoryQuest(quest)) {
            // A mission-board or other story-manager quest: no script can start
            // it, and its targets are random per machine. Browsing a board (a
            // start alone) costs nothing; once the host sets a stage in it (the
            // mission was accepted), take the host's save, which holds the
            // quest with the host's targets.
            if (p.stage == kNoStage) {
                PLOG_INFO("[PULSAR] QuestSync: quest=0x{:08X} can't be started here (story-manager quest) - waiting for the host to use it"sv, p.id);
            } else if (Pulsar::GetConfig().coopMissionBridge) {
                char why[96];
                std::snprintf(why, sizeof(why), "story-manager quest %08X stage %u (mission)", p.id, p.stage);
                Pulsar::TransitSync::RequestHostBridgeFromClient(why);
                PLOG_INFO("[PULSAR] QuestSync: quest=0x{:08X} stage={} is a story-manager quest not running here - asked the host for its save"sv,
                          p.id, p.stage);
            } else {
                PLOG_INFO("[PULSAR] QuestSync: quest=0x{:08X} stage={} is a story-manager quest not running here - CoopMissionBridge = 0, dropped"sv,
                          p.id, p.stage);
            }
            continue;
        }
        StartViaPapyrus(quest, p.id, p.stage);
    }
}

std::string RecentStartsForTest()
{
    std::lock_guard g(s_recentStartMutex);
    std::string s;
    for (const auto& [id, ms] : s_recentStarts) {
        char c[40];
        std::snprintf(c, sizeof(c), "%s%08X:%llu", s.empty() ? "" : ",", id, static_cast<unsigned long long>(ms));
        s += c;
    }
    return s;
}

bool IsRunningForTest(std::uint32_t a_id)
{
    auto* form = RE::TESForm::LookupByID(a_id);
    return form && form->GetFormType() == RE::FormType::kQUST && SafeIsRunning(static_cast<RE::TESQuest*>(form));
}

bool StartForTest(std::uint32_t a_id)
{
    auto* form = RE::TESForm::LookupByID(a_id);
    if (!form || form->GetFormType() != RE::FormType::kQUST) return false;
    auto* vm = Pulsar::GameCalls::VM();
    auto obj = Pulsar::GameCalls::ObjectFor(static_cast<RE::TESQuest*>(form));
    return vm && obj && Pulsar::GameCalls::CallMethod(vm, obj, "Start", {});
}

std::vector<std::uint32_t> RunningStoryQuestsForTest(const char* a_prefix, std::size_t a_max)
{
    std::string pre = a_prefix ? a_prefix : "";
    for (auto& c : pre) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    std::vector<std::uint32_t> out;
    for (std::uint32_t id = 0x800; id < 0x00400000 && out.size() < a_max; ++id) {
        auto* f = RE::TESForm::LookupByID(id);
        if (!f || f->GetFormType() != RE::FormType::kQUST) continue;
        auto* q = static_cast<RE::TESQuest*>(f);
        if (!SafeIsRunning(q) || !SafeIsStoryQuest(q)) continue;
        const char* eid = f->GetFormEditorID();
        if (!eid) continue;
        std::string low = eid;
        for (auto& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (low.rfind(pre, 0) == 0) out.push_back(id);
    }
    return out;
}

bool StopQuietlyForTest(std::uint32_t a_id)
{
    auto* form = RE::TESForm::LookupByID(a_id);
    if (!form || form->GetFormType() != RE::FormType::kQUST) return false;
    {
        std::lock_guard g(s_quietStopMutex);
        s_quietStop[a_id] = true;
    }
    auto* vm = Pulsar::GameCalls::VM();
    auto obj = Pulsar::GameCalls::ObjectFor(static_cast<RE::TESQuest*>(form));
    return vm && obj && Pulsar::GameCalls::CallMethod(vm, obj, "Stop", {});
}

namespace {
    std::mutex                                         s_stageCacheMutex;
    std::unordered_map<std::uint32_t, int>             s_stageCache;   // -1 unknown

    // Papyrus SetStage(a_stages[a_i]); on false try the next one. The stage
    // list of a mission is not known here, so the host tries a few likely ones.
    void TrySetStage(RE::BSScript::IVirtualMachine* a_vm, RE::BSTSmartPointer<RE::BSScript::Object> a_obj,
                     std::uint32_t a_id, std::vector<int> a_stages, std::size_t a_i)
    {
        if (a_i >= a_stages.size()) {
            PLOG_INFO("[PULSAR] QuestSync test: quest=0x{:08X} took none of {} stages"sv, a_id, a_stages.size());
            return;
        }
        const int st = a_stages[a_i];
        Pulsar::GameCalls::CallMethodForResult(a_vm, a_obj, "SetStage", { Pulsar::GameCalls::Arg(st) },
            [a_vm, a_obj, a_id, a_stages, a_i, st](const RE::BSScript::Variable& a_ok) {
                const bool ok = a_ok.is<bool>() && RE::BSScript::get<bool>(a_ok);
                PLOG_INFO("[PULSAR] QuestSync test: quest=0x{:08X} SetStage({}) = {}"sv, a_id, st, ok);
                if (!ok) TrySetStage(a_vm, a_obj, a_id, a_stages, a_i + 1);
            });
    }
}

bool AdvanceStageForTest(std::uint32_t a_id)
{
    auto* form = RE::TESForm::LookupByID(a_id);
    if (!form || form->GetFormType() != RE::FormType::kQUST) return false;
    auto* vm = Pulsar::GameCalls::VM();
    auto obj = Pulsar::GameCalls::ObjectFor(static_cast<RE::TESQuest*>(form));
    if (!vm || !obj) return false;
    return Pulsar::GameCalls::CallMethodForResult(vm, obj, "GetStage", {},
        [vm, obj, a_id](const RE::BSScript::Variable& a_cur) {
            const int cur = a_cur.is<std::int32_t>() ? RE::BSScript::get<std::int32_t>(a_cur) : 0;
            std::vector<int> stages;
            for (int d : { 10, 5, 1, 20, 15, 25, 50, 100 }) stages.push_back(cur + d);
            PLOG_INFO("[PULSAR] QuestSync test: quest=0x{:08X} at stage {}, advancing"sv, a_id, cur);
            TrySetStage(vm, obj, a_id, stages, 0);
        });
}

void RefreshStageForTest(std::uint32_t a_id)
{
    auto* form = RE::TESForm::LookupByID(a_id);
    if (!form || form->GetFormType() != RE::FormType::kQUST) return;
    auto* vm = Pulsar::GameCalls::VM();
    auto obj = Pulsar::GameCalls::ObjectFor(static_cast<RE::TESQuest*>(form));
    if (!vm || !obj) return;
    Pulsar::GameCalls::CallMethodForResult(vm, obj, "GetStage", {},
        [a_id](const RE::BSScript::Variable& a_v) {
            const int st = a_v.is<std::int32_t>() ? RE::BSScript::get<std::int32_t>(a_v) : -1;
            std::lock_guard g(s_stageCacheMutex);
            s_stageCache[a_id] = st;
        });
}

int CachedStageForTest(std::uint32_t a_id)
{
    std::lock_guard g(s_stageCacheMutex);
    auto it = s_stageCache.find(a_id);
    return it == s_stageCache.end() ? -1 : it->second;
}

std::vector<std::uint32_t> StoppedQuestsForTest(const char* a_hint, std::size_t a_max, std::size_t& a_seen)
{
    std::string hint = a_hint ? a_hint : "";
    for (auto& c : hint) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    std::vector<std::uint32_t> out;
    a_seen = 0;
    for (std::uint32_t id = 0x800; id < 0x00400000 && out.size() < a_max; ++id) {
        auto* f = RE::TESForm::LookupByID(id);
        if (!f || f->GetFormType() != RE::FormType::kQUST) continue;
        ++a_seen;
        if (SafeIsRunning(static_cast<RE::TESQuest*>(f))) continue;
        const char* eid = f->GetFormEditorID();
        if (!eid) continue;
        std::string low = eid;
        for (auto& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (low.find(hint) != std::string::npos) out.push_back(id);
    }
    return out;
}

}  // namespace Pulsar::QuestSync
