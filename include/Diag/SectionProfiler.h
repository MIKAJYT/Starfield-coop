#pragma once

// Time per named section of PULSAR's per-frame task (docs/PERFORMANCE.md). Game-free.
// One thread only (the game thread). Section names must be string literals: they are
// keyed by pointer.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Pulsar::Diag {

    class SectionProfiler
    {
    public:
        struct Section { const char* name; std::uint64_t us; std::uint64_t calls; std::uint64_t maxUs; };

        void Add(const char* a_name, std::uint64_t a_us)
        {
            for (auto& s : m_sections) {
                if (s.name == a_name) {
                    s.us += a_us;
                    ++s.calls;
                    s.maxUs = (std::max)(s.maxUs, a_us);
                    return;
                }
            }
            m_sections.push_back({ a_name, a_us, 1, a_us });
        }

        // The a_top sections that took longest, longest first; resets every section.
        std::vector<Section> Take(std::size_t a_top)
        {
            std::vector<Section> out;
            for (auto& s : m_sections) {
                if (s.calls) out.push_back(s);
                s.us = s.calls = s.maxUs = 0;
            }
            std::sort(out.begin(), out.end(), [](const Section& a, const Section& b) { return a.us > b.us; });
            if (out.size() > a_top) out.resize(a_top);
            return out;
        }

    private:
        std::vector<Section> m_sections;
    };

    // "NPCSync 210.3 ms/s (412 calls, max 4.10 ms), ..."
    inline std::string FormatSections(const std::vector<SectionProfiler::Section>& a_s, double a_seconds)
    {
        if (a_seconds <= 0) a_seconds = 1;
        std::string out;
        char buf[160];
        for (std::size_t i = 0; i < a_s.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s%s %.1f ms/s (%.0f/s, max %.2f ms)", i ? ", " : "", a_s[i].name,
                a_s[i].us / 1000.0 / a_seconds, a_s[i].calls / a_seconds, a_s[i].maxUs / 1000.0);
            out += buf;
        }
        return out;
    }

}  // namespace Pulsar::Diag
