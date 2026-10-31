// The cycle collector's precondition (A92, src/interpreter/cycle_detector.h).
//
// A collection marks from one engine's roots and clears every unreachable list
// whose refcount is above 1. A value another engine is using looks exactly like
// that to the sweep, so detectAndCollect() runs only when its engine is the only
// activity in the process. Each test below holds a list nothing marks, twice --
// the shape a live value of another engine has -- and checks that the collector
// leaves it alone for each reason it must defer, and clears it once the reason is
// gone. That second half is the control: it shows the probe list is one the
// sweep WOULD clear, so its survival is the gate's doing.
//
// The NAAb-level arms (two engines collecting at once) are
// tests/vm/test_gc_engine_activity.sh; these isolate the pieces a program cannot
// reach on its own: a second engine on the same thread, a pending engine, and a
// nested activation of the collecting engine.

#include <gtest/gtest.h>

#include "../../src/interpreter/cycle_detector.h"
#include "naab/compiler.h"
#include "naab/interpreter.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/stdlib.h"
#include "naab/vm.h"

#include <condition_variable>
#include <mutex>
#include <thread>

using naab::interpreter::CycleDetector;
using naab::interpreter::EngineActivation;
using naab::interpreter::NaabVal;
using naab::interpreter::PendingEngine;

namespace {

// A list held twice and reachable from no root: the sweep's idea of a cycle.
struct HeldTwice {
    NaabVal a = NaabVal::makeList({NaabVal::makeInt(1), NaabVal::makeInt(2),
                                   NaabVal::makeInt(3)});
    NaabVal b = a;
    size_t size() { return a.asList().size(); }
};

// Another thread executing an engine, until stop().
class ActiveElsewhere {
public:
    ActiveElsewhere() {
        thread_ = std::thread([this] {
            static int other_engine;
            EngineActivation active(&other_engine);
            std::unique_lock<std::mutex> lock(m_);
            started_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return stop_; });
        });
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return started_; });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    ~ActiveElsewhere() { stop(); }

private:
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool started_ = false;
    bool stop_ = false;
};

}  // namespace

TEST(GcEngineActivity, ProbeListIsClearedWhenTheCollectorIsAlone) {
    CycleDetector gc;
    HeldTwice probe;
    ASSERT_EQ(probe.a.getHeapRefCount(), 2);
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsRun(), 1u);
    EXPECT_EQ(gc.getCollectionsDeferred(), 0u);
    EXPECT_EQ(probe.size(), 0u) << "the probe must be a value the sweep clears";
}

TEST(GcEngineActivity, DefersWhileAnotherThreadExecutesAnEngine) {
    CycleDetector gc;
    HeldTwice probe;
    ActiveElsewhere other;
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsRun(), 0u);
    EXPECT_EQ(gc.getCollectionsDeferred(), 1u);
    EXPECT_EQ(probe.size(), 3u);

    other.stop();
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsRun(), 1u);
    EXPECT_EQ(probe.size(), 0u);
}

TEST(GcEngineActivity, DefersWhileAnEngineIsPending) {
    CycleDetector gc;
    HeldTwice probe;
    PendingEngine pending;
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsDeferred(), 1u);
    EXPECT_EQ(probe.size(), 3u);

    pending.release();
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsRun(), 1u);
    EXPECT_EQ(probe.size(), 0u);
}

TEST(GcEngineActivity, AMovedPendingEngineStillCountsOnce) {
    // The async spawn moves its token into the worker's lambda, then into the
    // body: the moved-from token must not release, the last holder must.
    CycleDetector gc;
    HeldTwice probe;
    PendingEngine first;
    PendingEngine second = std::move(first);
    {
        PendingEngine third = std::move(second);
        gc.detectAndCollect(nullptr, {}, {});
        EXPECT_EQ(gc.getCollectionsDeferred(), 1u);
        EXPECT_EQ(probe.size(), 3u);
    }
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsRun(), 1u);
    EXPECT_EQ(probe.size(), 0u);
}

TEST(GcEngineActivity, DefersWhenAnotherEngineIsActiveOnThisThread) {
    // A module VM runs inside its importer's run(), on the same thread.
    CycleDetector gc;
    HeldTwice probe;
    {
        static int importer;
        EngineActivation outer(&importer);
        EngineActivation inner(&gc);
        gc.detectAndCollect(nullptr, {}, {});
        EXPECT_EQ(gc.getCollectionsDeferred(), 1u);
        EXPECT_EQ(probe.size(), 3u);
    }
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsRun(), 1u);
    EXPECT_EQ(probe.size(), 0u);
}

TEST(GcEngineActivity, ANestedActivationOfTheCollectingEngineCollects) {
    // A callback re-enters the VM's own run() (a sort comparator, array.map_fn).
    CycleDetector gc;
    HeldTwice probe;
    EngineActivation outer(&gc);
    EngineActivation nested(&gc);
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsRun(), 1u);
    EXPECT_EQ(gc.getCollectionsDeferred(), 0u);
    EXPECT_EQ(probe.size(), 0u);
}

TEST(GcEngineActivity, ActivityEndsWithItsScope) {
    // Every activation and token released: a later collection runs.
    CycleDetector gc;
    {
        static int engine;
        EngineActivation a(&engine);
        EngineActivation b(&engine);
        PendingEngine p;
    }
    HeldTwice probe;
    gc.detectAndCollect(nullptr, {}, {});
    EXPECT_EQ(gc.getCollectionsRun(), 1u);
    EXPECT_EQ(probe.size(), 0u);
}

// Two VMs on two plain threads -- no async fn, so no PendingEngine is involved:
// only VM::run()'s own activation keeps one VM's collector from sweeping while
// the other runs. `hold` reads its list on every iteration (each read is a stack
// copy, refcount 2), `spin` only loops, and both loop long enough to reach the
// collection threshold many times. Before A92 the spinning VM cleared the
// holder's list in most runs (the NAAb-level version of this read 0 in 6 of 6).
namespace {
int runOnItsOwnVm(const std::string& src, const std::string& fn) {
    naab::lexer::Lexer lexer(src);
    auto tokens = lexer.tokenize();
    naab::parser::Parser parser(tokens);
    auto program = parser.parseProgram();
    naab::vm::Compiler compiler;
    auto* main_fn = compiler.compile(*program, "gc_engine_activity.naab");
    if (!main_fn) return -1;
    naab::stdlib::StdLib stdlib;
    naab::vm::VM vm;
    vm.setStdlib(&stdlib);
    vm.execute(main_fn);
    auto result = vm.callNaabFunction(vm.getGlobals().at(fn), {});
    return result.isInt() ? result.asInt() : -2;
}
}  // namespace

TEST(GcEngineActivity, AVmDoesNotSweepWhileAnotherVmRuns) {
    const std::string hold =
        "fn hold() {\n"
        "    let a = [1, 2, 3]\n"
        "    let s = 0\n"
        "    for i in 0..300000 {\n"
        "        s = s + a.length()\n"
        "    }\n"
        "    return a.length()\n"
        "}\n"
        "main { }\n";
    const std::string spin =
        "fn spin() {\n"
        "    let s = 0\n"
        "    for i in 0..300000 {\n"
        "        s = s + 1\n"
        "    }\n"
        "    return 3\n"
        "}\n"
        "main { }\n";
    for (int round = 0; round < 3; ++round) {
        int held = -3, spun = -3;
        std::thread a([&] { held = runOnItsOwnVm(hold, "hold"); });
        std::thread b([&] { spun = runOnItsOwnVm(spin, "spin"); });
        a.join();
        b.join();
        ASSERT_EQ(spun, 3) << "the spinning VM did not run";
        EXPECT_EQ(held, 3) << "round " << round << ": the other VM's collector "
                              "cleared a list this VM was using";
    }
}
