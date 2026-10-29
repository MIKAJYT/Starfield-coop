// Traffic and frame statistics (include/Diag/PerfStats.h).

#include "TestHarness.h"

#include "Diag/PerfStats.h"

using Pulsar::Diag::FrameStats;
using Pulsar::Diag::TrafficStats;

TEST(PerfStats, TrafficCountsByTypeAndResets)
{
    TrafficStats t;
    const std::uint8_t pos[40] = { 0x10 };
    const std::uint8_t npc[100] = { 0x30 };
    for (int i = 0; i < 3; ++i) t.CountSent(pos, sizeof(pos));
    t.CountSent(npc, sizeof(npc));
    t.CountReceived(pos, sizeof(pos));
    t.CountSent(nullptr, 5);   // ignored

    auto w = t.Take(1);
    CHECK(w.sentCount == 4);
    CHECK(w.sentBytes == 220);
    CHECK(w.recvCount == 1 && w.recvBytes == 40);
    CHECK(w.topSent.size() == 1);
    CHECK(w.topSent[0].type == 0x10);   // 120 bytes beat 100
    CHECK(w.topSent[0].count == 3);

    auto empty = t.Take(5);
    CHECK(empty.sentCount == 0 && empty.recvCount == 0 && empty.topSent.empty());
}

TEST(PerfStats, TrafficFormat)
{
    TrafficStats t;
    const std::uint8_t pos[1024] = { 0x10 };
    for (int i = 0; i < 10; ++i) t.CountSent(pos, sizeof(pos));
    const auto s = Pulsar::Diag::FormatTraffic(t.Take(3), 10.0);
    CHECK(s.find("up 1.0 KB/s (1 pkt/s)") != std::string::npos);
    CHECK(s.find("top 0x10 1.0 KB/s 1/s") != std::string::npos);
}

TEST(PerfStats, FramesAt60Fps)
{
    FrameStats f;
    std::uint64_t t = 1'000'000;
    for (int i = 0; i < 61; ++i) { f.Record(t, 200); t += 16'667; }
    auto w = f.Take();
    CHECK(w.frames == 60);
    CHECK(w.fps > 59.9 && w.fps < 60.1);
    CHECK(w.frameMsP50 > 16.6 && w.frameMsP50 < 16.7);
    CHECK(w.taskMsAvg > 0.19 && w.taskMsAvg < 0.21);
}

TEST(PerfStats, LoadScreenGapIsLeftOut)
{
    FrameStats f;
    std::uint64_t t = 1'000'000;
    for (int i = 0; i < 31; ++i) { f.Record(t, 100); t += 33'333; }
    t += 5'000'000;                 // a 5 s load screen
    f.Record(t, 9'000);
    auto w = f.Take();
    CHECK(w.frames == 30);
    CHECK(w.fps > 29.9 && w.fps < 30.1);
    CHECK(w.frameMsMax < 34);
    CHECK(w.taskMsMax > 8.9 && w.taskMsMax < 9.1);
}

TEST(PerfStats, P95PicksTheSlowFrames)
{
    FrameStats f;
    std::uint64_t t = 1'000'000;
    f.Record(t, 0);
    for (int i = 0; i < 100; ++i) { t += (i < 90) ? 10'000 : 50'000; f.Record(t, 0); }
    auto w = f.Take();
    CHECK(w.frameMsP50 > 9.9 && w.frameMsP50 < 10.1);
    CHECK(w.frameMsP95 > 49.9 && w.frameMsP95 < 50.1);
    CHECK(w.frameMsMax > 49.9);
}

#include "Diag/SectionProfiler.h"

TEST(PerfStats, SectionsSortedByTimeAndReset)
{
    Pulsar::Diag::SectionProfiler p;
    static const char* const kNpc = "NPCSync";
    static const char* const kLoot = "Loot";
    for (int i = 0; i < 10; ++i) p.Add(kNpc, 1000);
    p.Add(kLoot, 3000);
    p.Add(kLoot, 500);

    auto top = p.Take(5);
    CHECK(top.size() == 2);
    CHECK(top[0].name == kNpc && top[0].us == 10000 && top[0].calls == 10);
    CHECK(top[1].name == kLoot && top[1].maxUs == 3000);
    CHECK(Pulsar::Diag::FormatSections(top, 10.0).find("NPCSync 1.0 ms/s (1/s, max 1.00 ms)") != std::string::npos);

    CHECK(p.Take(5).empty());
    p.Add(kLoot, 7);
    CHECK(p.Take(1).size() == 1);
}
