#pragma once

// Which local NPC stands for a host NPC on the client (NPCSync Phase D
// matcher). Game-free so tests/test_match_policy.cpp can cover it.
//
// A save-file reference (FormID below 0xFF000000) is the same reference on
// both machines: the host's 002146E0 IS the client's 002146E0. The old matcher
// paired by template + nearest position only, and in Akila City three guards
// sharing template 0026FB62 came out rotated (002146E0 -> 002146EA -> 002146EF
// -> 002146E0, run 20261028-091213), so a host kill of one guard killed
// another on the client. Rules:
//   1. Identity first: the candidate with the host's own FormID, at any
//      distance, when it is loaded on the client.
//   2. A save-file candidate never stands for a DIFFERENT host reference: its
//      own host counterpart has its FormID.
//   3. Otherwise (runtime 0xFF references, whose IDs differ per machine) the
//      nearest unclaimed candidate with the same template within the radius.

#include <cstdint>
#include <cstddef>
#include <unordered_set>

namespace Pulsar::NPCSync
{
    struct MatchCandidate {
        std::uint32_t ref;             // local reference FormID
        std::uint32_t effectiveBase;   // template (source base) or base FormID
        float         x, y, z;
    };

    constexpr bool IsSaveFileRef(std::uint32_t a_formID) noexcept
    {
        return a_formID != 0 && (a_formID & 0xFF000000u) != 0xFF000000u;
    }

    inline std::uint32_t ChooseLocalMatch(std::uint32_t a_hostRef, std::uint32_t a_hostEffectiveBase,
                                          float a_hx, float a_hy, float a_hz,
                                          const MatchCandidate* a_cands, std::size_t a_count,
                                          const std::unordered_set<std::uint32_t>& a_claimed,
                                          float a_radius = 50.0f)
    {
        if (a_hostEffectiveBase == 0 || (!a_cands && a_count)) return 0;
        if (IsSaveFileRef(a_hostRef)) {
            for (std::size_t i = 0; i < a_count; ++i)
                if (a_cands[i].ref == a_hostRef) return a_hostRef;
        }
        std::uint32_t best = 0;
        float bestD2 = a_radius * a_radius;
        for (std::size_t i = 0; i < a_count; ++i) {
            const auto& c = a_cands[i];
            if (c.effectiveBase != a_hostEffectiveBase) continue;
            if (IsSaveFileRef(c.ref)) continue;   // rule 2 (its own ID was rule 1)
            if (a_claimed.count(c.ref)) continue;
            const float dx = c.x - a_hx, dy = c.y - a_hy, dz = c.z - a_hz;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD2) { bestD2 = d2; best = c.ref; }
        }
        return best;
    }
}
