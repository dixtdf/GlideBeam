#include "../latency-stats.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0;
static void Check(bool condition, const char* name) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}

int main() {
    RollingLatencyStats stats;
    auto empty = stats.Snapshot();
    Check(!empty.totalSamples && !empty.windowSamples && !empty.meanUs, "empty window");
    for (uint64_t i = 1; i <= 100; ++i) stats.ObserveUs(i);
    auto first = stats.Snapshot();
    Check(first.totalSamples == 100 && first.windowSamples == 100 && first.lastUs == 100,
          "partial window counts and last sample");
    Check(first.meanUs == 50.5 && first.p95Us == 95 && first.p99Us == 99 && first.maxUs == 100,
          "nearest-rank percentiles");
    for (uint64_t i = 101; i <= 300; ++i) stats.ObserveUs(i);
    auto wrapped = stats.Snapshot();
    Check(wrapped.totalSamples == 300 && wrapped.windowSamples == 256 && wrapped.lastUs == 300,
          "bounded rolling window");
    Check(wrapped.meanUs == 172.5 && wrapped.p95Us == 288 && wrapped.p99Us == 298,
          "evicted samples excluded from percentiles");
    stats.Reset();
    Check(!stats.Snapshot().totalSamples && !stats.Snapshot().windowSamples, "reset drops old samples");
    stats.ObserveUs((std::numeric_limits<uint64_t>::max)());
    stats.ObserveUs((std::numeric_limits<uint64_t>::max)());
    Check(std::isfinite(stats.Snapshot().meanUs) && stats.Snapshot().p99Us == (std::numeric_limits<uint64_t>::max)(),
          "duration summation does not overflow");
    stats.Reset();

    std::atomic<bool> start{false};
    std::atomic<unsigned> completed{0};
    std::vector<std::thread> writers;
    for (unsigned i = 1; i <= 4; ++i) writers.emplace_back([&, i] {
        while (!start.load()) std::this_thread::yield();
        for (unsigned n = 0; n < 10000; ++n) stats.ObserveUs(i * 100);
        completed.fetch_add(1);
    });
    start.store(true);
    uint64_t previousCount = 0;
    while (completed.load() < 4) {
        auto current = stats.Snapshot();
        Check(current.totalSamples >= previousCount && current.windowSamples <= 256 &&
              current.windowSamples <= current.totalSamples, "coherent concurrent snapshot");
        if (current.windowSamples) Check(current.meanUs >= 100 && current.meanUs <= 400 &&
              current.p95Us <= current.p99Us && current.p99Us <= current.maxUs && current.maxUs <= 400,
              "concurrent percentile bounds");
        previousCount = current.totalSamples;
        std::this_thread::yield();
    }
    for (auto& writer : writers) writer.join();
    Check(stats.Snapshot().totalSamples == 40000, "all concurrent observations retained in count");
    stats.Reset();
    stats.ObserveUs(7);
    auto single = stats.Snapshot();
    Check(single.windowSamples == 1 && single.meanUs == 7 && single.p95Us == 7 && single.p99Us == 7,
          "one new sample after reset");
    if (failures) return 1;
    std::puts("Latency statistics PASS (rolling percentiles, reset, overflow, concurrency)");
    return 0;
}
