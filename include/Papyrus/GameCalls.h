#pragma once

// Small helpers for calling vanilla Papyrus functions from C++ and walking the actors
// near the player. Used by the solo buddy (Debug/Buddy) and the figures of players 2-4
// (RemotePlayer). Game thread.

#include <functional>
#include <vector>

namespace Pulsar::GameCalls {

    RE::BSScript::IVirtualMachine* VM();

    // Script object for a form (binds one if the form has none yet).
    template <class T>
    RE::BSTSmartPointer<RE::BSScript::Object> ObjectFor(T* a_form)
    {
        RE::BSScript::Variable v;
        RE::BSScript::PackVariable(v, a_form);
        if (!v.is<RE::BSScript::Object>()) return nullptr;
        return RE::BSScript::get<RE::BSScript::Object>(v);
    }

    template <class T>
    RE::BSScript::Variable Arg(T a_val)
    {
        RE::BSScript::Variable v;
        RE::BSScript::PackVariable(v, a_val);
        return v;
    }

    // Queue a method call on the VM. False if the VM rejected it (unknown function or
    // wrong argument list); the call itself runs later.
    bool CallMethod(RE::BSScript::IVirtualMachine* a_vm,
                    const RE::BSTSmartPointer<RE::BSScript::Object>& a_self,
                    const char* a_func,
                    std::vector<RE::BSScript::Variable> a_args);

    // Like CallMethod, and a_onResult gets the function's return value once the VM ran it
    // (on a VM thread).
    bool CallMethodForResult(RE::BSScript::IVirtualMachine* a_vm,
                             const RE::BSTSmartPointer<RE::BSScript::Object>& a_self,
                             const char* a_func,
                             std::vector<RE::BSScript::Variable> a_args,
                             std::function<void(const RE::BSScript::Variable&)> a_onResult);

    // A global (static) script function, with its return value (on a VM thread).
    bool CallStaticForResult(RE::BSScript::IVirtualMachine* a_vm,
                             const char* a_script,
                             const char* a_func,
                             std::vector<RE::BSScript::Variable> a_args,
                             std::function<void(const RE::BSScript::Variable&)> a_onResult);

    // Activate a_target as the player through the engine Activate core (REL::ID
    // 63380), as the interact key does. 1 activated, 0 refused, -1 fault. Game thread.
    int ActivateAsPlayer(RE::TESObjectREFR* a_target);

    // Every actor in the high process list (the ones near the player), SEH-guarded.
    void ForEachHighActor(const std::function<void(RE::Actor*)>& a_fn);

}  // namespace Pulsar::GameCalls
