// bench_governance.cpp — Performance benchmarks for the governance engine
//
// Measures: single scan latency, throughput, concurrent scaling
// No external framework — uses std::chrono only.
//
// Build: cmake .. && make bench_governance
// Run:   ./build/bench_governance

#include "naab/governance.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace naab::governance;
using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Test vectors
// ---------------------------------------------------------------------------

static const char* CLEAN_PYTHON =
    "def add(a, b):\n"
    "    return a + b\n"
    "\n"
    "result = add(1, 2)\n"
    "print(result)\n";

static const char* VIOLATION_PYTHON =
    "import os\n"
    "os.system('rm -rf /')\n"
    "eval(input())\n";

// ~100 lines of realistic Python
static const char* MEDIUM_PYTHON =
    "import json\n"
    "import math\n"
    "from typing import List, Dict, Optional\n"
    "\n"
    "class DataProcessor:\n"
    "    def __init__(self, config: Dict):\n"
    "        self.config = config\n"
    "        self.results = []\n"
    "        self.cache = {}\n"
    "\n"
    "    def process(self, data: List[Dict]) -> List[Dict]:\n"
    "        output = []\n"
    "        for item in data:\n"
    "            if not self.validate(item):\n"
    "                continue\n"
    "            transformed = self.transform(item)\n"
    "            if transformed:\n"
    "                output.append(transformed)\n"
    "        self.results.extend(output)\n"
    "        return output\n"
    "\n"
    "    def validate(self, item: Dict) -> bool:\n"
    "        required = ['id', 'name', 'value']\n"
    "        return all(k in item for k in required)\n"
    "\n"
    "    def transform(self, item: Dict) -> Optional[Dict]:\n"
    "        key = item['id']\n"
    "        if key in self.cache:\n"
    "            return self.cache[key]\n"
    "        result = {\n"
    "            'id': item['id'],\n"
    "            'name': item['name'].strip().lower(),\n"
    "            'score': math.sqrt(abs(item['value'])),\n"
    "            'grade': self.compute_grade(item['value']),\n"
    "        }\n"
    "        self.cache[key] = result\n"
    "        return result\n"
    "\n"
    "    def compute_grade(self, value: float) -> str:\n"
    "        if value >= 90: return 'A'\n"
    "        if value >= 80: return 'B'\n"
    "        if value >= 70: return 'C'\n"
    "        if value >= 60: return 'D'\n"
    "        return 'F'\n"
    "\n"
    "    def summary(self) -> Dict:\n"
    "        if not self.results:\n"
    "            return {'count': 0, 'mean': 0.0}\n"
    "        scores = [r['score'] for r in self.results]\n"
    "        return {\n"
    "            'count': len(scores),\n"
    "            'mean': sum(scores) / len(scores),\n"
    "            'min': min(scores),\n"
    "            'max': max(scores),\n"
    "        }\n"
    "\n"
    "def main():\n"
    "    config = {'threshold': 0.5, 'max_items': 1000}\n"
    "    processor = DataProcessor(config)\n"
    "    data = [{'id': i, 'name': f'item_{i}', 'value': i * 1.5} for i in range(100)]\n"
    "    results = processor.process(data)\n"
    "    print(json.dumps(processor.summary(), indent=2))\n"
    "\n"
    "if __name__ == '__main__':\n"
    "    main()\n";

static const char* TEST_CONFIG = R"({
    "version": "3.0",
    "mode": "enforce",
    "restrictions": {
        "dangerous_calls": {"level": "hard"},
        "shell_injection": {"level": "hard"},
        "code_injection": {"level": "hard"}
    },
    "code_quality": {
        "semantic_checks": {"level": "hard"},
        "no_secrets": {"level": "hard"}
    }
})";

// ---------------------------------------------------------------------------
// Stats helper
// ---------------------------------------------------------------------------

struct Stats {
    double median_us;
    double p95_us;
    double p99_us;
    double min_us;
    double max_us;
    double mean_us;
};

static Stats computeStats(std::vector<double>& times) {
    std::sort(times.begin(), times.end());
    size_t n = times.size();
    double sum = 0;
    for (double t : times) sum += t;
    return {
        times[n / 2],
        times[(size_t)(n * 0.95)],
        times[(size_t)(n * 0.99)],
        times[0],
        times[n - 1],
        sum / n
    };
}

static void printStats(const char* label, const Stats& s, int iterations) {
    printf("  %-24s  n=%-6d  median=%.1fus  p95=%.1fus  p99=%.1fus  min=%.1fus  max=%.1fus\n",
           label, iterations, s.median_us, s.p95_us, s.p99_us, s.min_us, s.max_us);
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

static Stats benchSingleScan(const char* code, int iterations) {
    GovernanceEngine engine;
    engine.loadFromString(TEST_CONFIG);

    std::vector<double> times;
    times.reserve(iterations);

    for (int i = 0; i < iterations; i++) {
        engine.resetCheckResults();
        auto start = Clock::now();
        engine.checkPolyglotBlock("python", code, "bench.py", 1);
        auto end = Clock::now();
        times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }

    return computeStats(times);
}

static void benchThroughput(const char* label, const char* code, int duration_sec) {
    GovernanceEngine engine;
    engine.loadFromString(TEST_CONFIG);

    auto deadline = Clock::now() + std::chrono::seconds(duration_sec);
    int count = 0;
    auto start = Clock::now();

    while (Clock::now() < deadline) {
        engine.resetCheckResults();
        engine.checkPolyglotBlock("python", code, "bench.py", 1);
        count++;
    }

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    printf("  %-24s  %d scans in %.1fs = %.0f scans/sec\n",
           label, count, elapsed, count / elapsed);
}

static void benchConcurrent(int thread_count, const char* code, int per_thread) {
    std::atomic<int> total{0};
    auto start = Clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&total, code, per_thread]() {
            GovernanceEngine engine;
            engine.loadFromString(TEST_CONFIG);
            for (int i = 0; i < per_thread; i++) {
                engine.resetCheckResults();
                engine.checkPolyglotBlock("python", code, "bench.py", 1);
                total.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    printf("  %d threads x %d scans   %d total in %.2fs = %.0f scans/sec\n",
           thread_count, per_thread, total.load(), elapsed, total.load() / elapsed);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    printf("=== NAAb Governance Engine Benchmarks ===\n\n");

    // Warmup
    printf("Warmup (100 iterations)...\n");
    benchSingleScan(CLEAN_PYTHON, 100);
    printf("  done\n\n");

    // Detect platform speed: run 1 scan, scale iterations accordingly
    double per_scan_ms;
    {
        GovernanceEngine probe;
        probe.loadFromString(TEST_CONFIG);
        auto t0 = Clock::now();
        probe.resetCheckResults();
        probe.checkPolyglotBlock("python", CLEAN_PYTHON, "probe.py", 1);
        per_scan_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        printf("  Probe: %.2f ms/scan\n\n", per_scan_ms);
    }

    // Scale iterations based on platform speed
    // Fast (< 1ms): 5000 iters.  Medium (1-100ms): 100 iters.  Slow (> 100ms): 10 iters.
    int iters, per_thread, throughput_sec;
    if (per_scan_ms < 1.0) {
        iters = 5000; per_thread = 2000; throughput_sec = 3;
    } else if (per_scan_ms < 10.0) {
        iters = 500; per_thread = 200; throughput_sec = 2;
    } else if (per_scan_ms < 100.0) {
        iters = 100; per_thread = 50; throughput_sec = 2;
    } else {
        iters = 10; per_thread = 5; throughput_sec = 1;
    }

    // 1. Single scan latency
    printf("--- Single Scan Latency (n=%d) ---\n", iters);
    printStats("clean_code",     benchSingleScan(CLEAN_PYTHON, iters), iters);
    printStats("violation_code", benchSingleScan(VIOLATION_PYTHON, iters), iters);
    printStats("medium_code",    benchSingleScan(MEDIUM_PYTHON, iters), iters);
    printf("\n");

    // 2. Throughput
    printf("--- Sustained Throughput (%ds each) ---\n", throughput_sec);
    benchThroughput("clean_code", CLEAN_PYTHON, throughput_sec);
    benchThroughput("medium_code", MEDIUM_PYTHON, throughput_sec);
    printf("\n");

    // 3. Concurrent scaling
    printf("--- Concurrent Scaling (%d/thread) ---\n", per_thread);
    benchConcurrent(1, CLEAN_PYTHON, per_thread);
    benchConcurrent(2, CLEAN_PYTHON, per_thread);
    benchConcurrent(4, CLEAN_PYTHON, per_thread);
    printf("\n");

    printf("=== Benchmarks Complete ===\n");
    return 0;
}
