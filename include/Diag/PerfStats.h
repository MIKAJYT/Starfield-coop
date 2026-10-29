#pragma once

// Traffic and frame statistics for the periodic "NetStats" / "FrameStats" log lines
// (docs/PERFORMANCE.md). Game-free: the counters are fed from NetworkHost (packets) and
// the per-frame SFSE task (frame times), and read out once per window.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Pulsar::Diag {

    // Application-level packet counters by packet type (first payload byte), before
    // encryption. Written from the game thread and the network thread.
    class TrafficStats
    {
    public:
        struct Type { std::uint8_t type; std::uint64_t count; std::uint64_t bytes; };
        struct Window
        {
            std::uint64_t sentCount = 0, sentBytes = 0, recvCount = 0, recvBytes = 0;
            std::vector<Type> topSent;   // most bytes first
        };

        void CountSent(const std::uint8_t* a_data, std::size_t a_len) noexcept
        {
            if (!a_data || !a_len) return;
            m_sentCount[a_data[0]].fetch_add(1, std::memory_order_relaxed);
            m_sentBytes[a_data[0]].fetch_add(a_len, std::memory_order_relaxed);
        }
        void CountReceived(const std::uint8_t* a_data, std::size_t a_len) noexcept
        {
            if (!a_data || !a_len) return;
            m_recvCount.fetch_add(1, std::memory_order_relaxed);
            m_recvBytes.fetch_add(a_len, std::memory_order_relaxed);
        }

        // Reads and resets the counters. a_top: how many packet types to list.
        Window Take(std::size_t a_top) noexcept
        {
            Window w;
            std::vector<Type> all;
            for (std::size_t i = 0; i < 256; ++i) {
                const auto c = m_sentCount[i].exchange(0, std::memory_order_relaxed);
                const auto b = m_sentBytes[i].exchange(0, std::memory_order_relaxed);
                if (!c) continue;
                w.sentCount += c;
                w.sentBytes += b;
                all.push_back({ static_cast<std::uint8_t>(i), c, b });
            }
            std::sort(all.begin(), all.end(), [](const Type& a, const Type& b) { return a.bytes > b.bytes; });
            if (all.size() > a_top) all.resize(a_top);
            w.topSent = std::move(all);
            w.recvCount = m_recvCount.exchange(0, std::memory_order_relaxed);
            w.recvBytes = m_recvBytes.exchange(0, std::memory_order_relaxed);
            return w;
        }

    private:
        std::array<std::atomic<std::uint64_t>, 256> m_sentCount{};
        std::array<std::atomic<std::uint64_t>, 256> m_sentBytes{};
        std::atomic<std::uint64_t> m_recvCount{ 0 }, m_recvBytes{ 0 };
    };

    // Frame times: the interval between two presented frames (the test harness feeds
    // it from IDXGISwapChain::Present) and, optionally, how long some work in each
    // frame took. Not thread-safe.
    class FrameStats
    {
    public:
        static constexpr std::size_t kMaxSamples = 8192;

        struct Window
        {
            std::size_t   frames = 0;
            double        fps = 0, frameMsP50 = 0, frameMsP95 = 0, frameMsMax = 0;
            double        taskMsAvg = 0, taskMsMax = 0;
        };

        // a_startUs: task start (microseconds, monotonic); a_taskUs: how long the task ran.
        void Record(std::uint64_t a_startUs, std::uint64_t a_taskUs)
        {
            if (m_lastStartUs && a_startUs > m_lastStartUs && m_frameUs.size() < kMaxSamples)
                m_frameUs.push_back(a_startUs - m_lastStartUs);
            m_lastStartUs = a_startUs;
            m_taskUsSum += a_taskUs;
            m_taskUsMax = (std::max)(m_taskUsMax, a_taskUs);
            ++m_tasks;
        }

        // Frames that took longer than a_gapUs (a load screen, a pause menu) are left
        // out of the frame-time figures, so a window across a load still reads right.
        Window Take(std::uint64_t a_gapUs = 1'000'000)
        {
            Window w;
            std::vector<std::uint64_t> f;
            f.reserve(m_frameUs.size());
            for (auto us : m_frameUs) if (us <= a_gapUs) f.push_back(us);
            if (!f.empty()) {
                std::uint64_t sum = 0;
                for (auto us : f) sum += us;
                std::sort(f.begin(), f.end());
                w.frames     = f.size();
                w.fps        = 1e6 * static_cast<double>(f.size()) / static_cast<double>(sum);
                w.frameMsP50 = f[(f.size() - 1) / 2] / 1000.0;
                w.frameMsP95 = f[(f.size() - 1) * 95 / 100] / 1000.0;
                w.frameMsMax = f.back() / 1000.0;
            }
            if (m_tasks) {
                w.taskMsAvg = static_cast<double>(m_taskUsSum) / static_cast<double>(m_tasks) / 1000.0;
                w.taskMsMax = m_taskUsMax / 1000.0;
            }
            m_frameUs.clear();
            m_taskUsSum = m_taskUsMax = m_tasks = 0;
            return w;
        }

    private:
        std::vector<std::uint64_t> m_frameUs;
        std::uint64_t m_lastStartUs = 0, m_taskUsSum = 0, m_taskUsMax = 0, m_tasks = 0;
    };

    // "up 12.3 KB/s (61 pkt/s), down 4.0 KB/s (30 pkt/s); top 0x10 8.1 KB/s 30/s, ..."
    inline std::string FormatTraffic(const TrafficStats::Window& a_w, double a_seconds)
    {
        if (a_seconds <= 0) a_seconds = 1;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "up %.1f KB/s (%.0f pkt/s), down %.1f KB/s (%.0f pkt/s)",
            a_w.sentBytes / 1024.0 / a_seconds, a_w.sentCount / a_seconds,
            a_w.recvBytes / 1024.0 / a_seconds, a_w.recvCount / a_seconds);
        std::string s = buf;
        for (std::size_t i = 0; i < a_w.topSent.size(); ++i) {
            const auto& t = a_w.topSent[i];
            std::snprintf(buf, sizeof(buf), "%s0x%02X %.1f KB/s %.0f/s", i ? ", " : "; top ",
                t.type, t.bytes / 1024.0 / a_seconds, t.count / a_seconds);
            s += buf;
        }
        return s;
    }

}  // namespace Pulsar::Diag
