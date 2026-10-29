#pragma once

// PROF / PROF_CALL: time a section of PULSAR's per-frame task, from any source file.
// The totals are logged as "TaskProfile" every 10 s (src/Main.cpp, TaskTimer). Dev builds
// only: in the hardened release build both macros compile to nothing.

#include "Diag/SectionProfiler.h"

#include <cstdint>

namespace Pulsar::Diag {

    SectionProfiler& TaskProfile();      // src/Main.cpp
    std::uint64_t    TaskProfileNowUs();  // src/Main.cpp

    struct ProfScope
    {
        explicit ProfScope(const char* a_name) : m_name(a_name), m_start(TaskProfileNowUs()) {}
        ~ProfScope() { TaskProfile().Add(m_name, TaskProfileNowUs() - m_start); }
        ProfScope(const ProfScope&) = delete;
        ProfScope& operator=(const ProfScope&) = delete;

        const char*   m_name;
        std::uint64_t m_start;
    };

}  // namespace Pulsar::Diag

#ifndef PULSAR_HARDENED
#    define PROF_CONCAT2(a, b) a##b
#    define PROF_CONCAT(a, b) PROF_CONCAT2(a, b)
#    define PROF(a_name) ::Pulsar::Diag::ProfScope PROF_CONCAT(prof_, __LINE__)(a_name)
// No object with a destructor, so it also works in functions that use __try (C2712).
#    define PROF_CALL(a_name, ...) do { const std::uint64_t prof_t0 = ::Pulsar::Diag::TaskProfileNowUs(); __VA_ARGS__; ::Pulsar::Diag::TaskProfile().Add(a_name, ::Pulsar::Diag::TaskProfileNowUs() - prof_t0); } while (0)
#else
#    define PROF(a_name) ((void)0)
#    define PROF_CALL(a_name, ...) do { __VA_ARGS__; } while (0)
#endif
