// Host-to-local NPC matching on the client (include/NPCSync/MatchPolicy.h).

#include "TestHarness.h"

#include "NPCSync/MatchPolicy.h"

#include <vector>

using namespace Pulsar::NPCSync;

namespace {
    constexpr std::uint32_t kGuardTemplate = 0x0026FB62u;
    // The three Akila City guards from run 20261028-091213, as the client saw
    // them: each host guard's position lies nearest to a DIFFERENT local guard.
    const std::vector<MatchCandidate> kGuards = {
        { 0x002146E0u, kGuardTemplate, -1920.0f, -655.0f, 32.0f },
        { 0x002146EAu, kGuardTemplate, -1935.0f, -647.0f, 32.0f },
        { 0x002146EFu, kGuardTemplate, -1926.0f, -656.0f, 32.0f },
    };
    const std::unordered_set<std::uint32_t> kNone;
}

TEST(MatchPolicy, SaveFileRefMatchesItselfNotTheNearest)
{
    // Host 002146E0 stands where the client has 002146EA: still 002146E0.
    CHECK_EQ(ChooseLocalMatch(0x002146E0u, kGuardTemplate, -1935, -647, 32, kGuards.data(), kGuards.size(), kNone),
             0x002146E0u);
    CHECK_EQ(ChooseLocalMatch(0x002146EAu, kGuardTemplate, -1926, -656, 32, kGuards.data(), kGuards.size(), kNone),
             0x002146EAu);
    CHECK_EQ(ChooseLocalMatch(0x002146EFu, kGuardTemplate, -1920, -655, 32, kGuards.data(), kGuards.size(), kNone),
             0x002146EFu);
}

TEST(MatchPolicy, SaveFileRefMatchesItselfAtAnyDistance)
{
    const std::vector<MatchCandidate> far = { { 0x002146E0u, kGuardTemplate, 0, 0, 0 } };
    CHECK_EQ(ChooseLocalMatch(0x002146E0u, kGuardTemplate, 5000, 5000, 0, far.data(), far.size(), kNone), 0x002146E0u);
}

TEST(MatchPolicy, NeverAnotherSaveFileRef)
{
    // The host guard's own copy is not loaded on the client: no match rather
    // than a different guard.
    const std::vector<MatchCandidate> others = { kGuards[1], kGuards[2] };
    CHECK_EQ(ChooseLocalMatch(0x002146E0u, kGuardTemplate, -1935, -647, 32, others.data(), others.size(), kNone), 0u);
    // A runtime host NPC never takes a save-file NPC either.
    CHECK_EQ(ChooseLocalMatch(0xFF00D558u, kGuardTemplate, -1935, -647, 32, kGuards.data(), kGuards.size(), kNone), 0u);
}

TEST(MatchPolicy, RuntimeRefsMatchByTemplateAndPosition)
{
    const std::vector<MatchCandidate> spawned = {
        { 0xFF00D5ADu, 0x00259DC6u, 10, 0, 0 },
        { 0xFF00D5B1u, 0x00259DC6u, 2, 0, 0 },
        { 0xFF00D5B5u, 0x00111111u, 0, 0, 0 },   // other template, nearest
    };
    CHECK_EQ(ChooseLocalMatch(0xFF00D558u, 0x00259DC6u, 0, 0, 0, spawned.data(), spawned.size(), kNone), 0xFF00D5B1u);
    // Exclusive: a claimed local is skipped.
    const std::unordered_set<std::uint32_t> claimed = { 0xFF00D5B1u };
    CHECK_EQ(ChooseLocalMatch(0xFF00D55Cu, 0x00259DC6u, 0, 0, 0, spawned.data(), spawned.size(), claimed), 0xFF00D5ADu);
    // Outside 50 m: none.
    CHECK_EQ(ChooseLocalMatch(0xFF00D558u, 0x00259DC6u, 500, 0, 0, spawned.data(), spawned.size(), kNone), 0u);
    // No template: none.
    CHECK_EQ(ChooseLocalMatch(0xFF00D558u, 0, 0, 0, 0, spawned.data(), spawned.size(), kNone), 0u);
}
