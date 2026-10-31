#include "PCH.h"
#include "Papyrus/GameCalls.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/P/ProcessLists.h>

#include <Windows.h>

using namespace std::string_view_literals;

namespace Pulsar::GameCalls {

    namespace {
        RE::Actor* ResolveHighActor(RE::ProcessLists* a_pl, std::uint32_t a_i)
        {
            RE::NiPointer<RE::Actor> sp = a_pl->highActorHandles[a_i].get();
            return sp.get();
        }

        RE::Actor* SafeResolveHighActor(RE::ProcessLists* a_pl, std::uint32_t a_i)
        {
            __try { return ResolveHighActor(a_pl, a_i); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        }

        bool SafeReadRawHandle(RE::ProcessLists* a_pl, std::uint32_t a_i, std::uint32_t& a_h)
        {
            __try {
                auto* base = reinterpret_cast<std::uint32_t*>(a_pl->highActorHandles.data());
                if (!base) return false;
                a_h = base[a_i];
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        std::uint32_t SafeHighActorCount(RE::ProcessLists* a_pl)
        {
            __try { return static_cast<std::uint32_t>(a_pl->highActorHandles.size()); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
    }

    RE::BSScript::IVirtualMachine* VM()
    {
        auto* game = RE::GameVM::GetSingleton();
        return game ? game->GetVM() : nullptr;
    }

    bool CallMethod(RE::BSScript::IVirtualMachine* a_vm,
                    const RE::BSTSmartPointer<RE::BSScript::Object>& a_self,
                    const char* a_func,
                    std::vector<RE::BSScript::Variable> a_args)
    {
        if (!a_vm || !a_self) return false;
        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(static_cast<std::uint32_t>(a_args.size()));
            for (std::uint32_t i = 0; i < a_args.size(); ++i) args[i] = a_args[i];
            return true;
        };
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        return a_vm->DispatchMethodCall(a_self, RE::BSFixedString(a_func), argFn, callback, 0);
    }

    namespace {
        // Receives a dispatched call's return value. Ref-counted by the VM's smart pointer.
        class ResultCallback final : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            explicit ResultCallback(std::function<void(const RE::BSScript::Variable&)> a_fn) : m_fn(std::move(a_fn)) {}
            void CallQueued() override {}
            void CallCanceled() override { PLOG_INFO("[PULSAR] GameCalls: a Papyrus call was canceled by the VM"sv); }
            void StartMultiDispatch() override {}
            void EndMultiDispatch() override {}
            void operator()(RE::BSScript::Variable a_result) override
            {
                if (m_fn) m_fn(a_result);
            }

        private:
            std::function<void(const RE::BSScript::Variable&)> m_fn;
        };
    }

    bool CallMethodForResult(RE::BSScript::IVirtualMachine* a_vm,
                             const RE::BSTSmartPointer<RE::BSScript::Object>& a_self,
                             const char* a_func,
                             std::vector<RE::BSScript::Variable> a_args,
                             std::function<void(const RE::BSScript::Variable&)> a_onResult)
    {
        if (!a_vm || !a_self) return false;
        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(static_cast<std::uint32_t>(a_args.size()));
            for (std::uint32_t i = 0; i < a_args.size(); ++i) args[i] = a_args[i];
            return true;
        };
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new ResultCallback(std::move(a_onResult)));
        return a_vm->DispatchMethodCall(a_self, RE::BSFixedString(a_func), argFn, callback, 0);
    }

    bool CallStaticForResult(RE::BSScript::IVirtualMachine* a_vm,
                             const char* a_script,
                             const char* a_func,
                             std::vector<RE::BSScript::Variable> a_args,
                             std::function<void(const RE::BSScript::Variable&)> a_onResult)
    {
        if (!a_vm) return false;
        auto argFn = [&](RE::BSScrapArray<RE::BSScript::Variable>& args) -> bool {
            args.resize(static_cast<std::uint32_t>(a_args.size()));
            for (std::uint32_t i = 0; i < a_args.size(); ++i) args[i] = a_args[i];
            return true;
        };
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new ResultCallback(std::move(a_onResult)));
        return a_vm->DispatchStaticCall(RE::BSFixedString(a_script), RE::BSFixedString(a_func), argFn, callback, 0);
    }

    namespace {
        // Argument block of the engine Activate core (REL::ID 63380, the function
        // Papyrus ObjectReference.Activate ends in; LootSync hooks it).
        struct ActivateArgs
        {
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

        int SafeActivate(ActivateFn a_fn, RE::TESObjectREFR* a_target, ActivateArgs* a_args) noexcept
        {
            __try {
                return a_fn(a_target, a_args) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }
    }

    int ActivateAsPlayer(RE::TESObjectREFR* a_target)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!a_target || !player) return 0;
        static const auto s_activate = reinterpret_cast<ActivateFn>(REL::Relocation<std::uintptr_t>{ REL::ID(63380) }.address());
        ActivateArgs args{};
        args.activator   = player;
        args.countOrFlag = 1;
        args.flag1d      = 1;
        return SafeActivate(s_activate, a_target, &args);
    }

    void ForEachHighActor(const std::function<void(RE::Actor*)>& a_fn)
    {
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) return;
        std::uint32_t count = SafeHighActorCount(pl);
        if (count > 512) count = 512;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t raw = 0;
            if (!SafeReadRawHandle(pl, i, raw) || raw == 0 || raw == 0xFFFFFFFFu) continue;
            if (auto* a = SafeResolveHighActor(pl, i)) a_fn(a);
        }
    }

}  // namespace Pulsar::GameCalls
