// Which VM a tool runs on (A93, include/naab/thread_vm.h).
//
// The NAAb-level arms (two batched tools at once, an async fn's tool beside
// main) are tests/governance_v4/test_tool_thread_vm.sh. These isolate what a
// program cannot reach on its own: a loan revoked while a worker still holds
// it, the scopes restoring what they replaced, and a lent VM entered from two
// threads at once -- the subject of the ThreadSanitizer job.

#include <gtest/gtest.h>

#include "naab/compiler.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/stdlib.h"
#include "naab/thread_vm.h"
#include "naab/vm.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using naab::interpreter::NaabVal;
using naab::runtime::LentVMScope;
using naab::runtime::RunningVMScope;
using naab::runtime::ThreadVM;
using naab::runtime::VMLoan;
using naab::runtime::currentThreadVM;

namespace {

// A VM holding one compiled program, for callToolOnThreadVM to enter.
struct ProgramVM {
    naab::stdlib::StdLib stdlib;
    naab::vm::VM vm;
    std::unique_ptr<naab::ast::Program> program;
    naab::vm::Compiler compiler;   // owns the compiled functions
    explicit ProgramVM(const std::string& src) {
        naab::lexer::Lexer lexer(src);
        auto tokens = lexer.tokenize();
        naab::parser::Parser parser(tokens);
        program = parser.parseProgram();
        auto* main_fn = compiler.compile(*program, "thread_vm.naab");
        if (!main_fn) throw std::runtime_error("compile failed");
        vm.setStdlib(&stdlib);
        vm.execute(main_fn);
    }
    NaabVal fn(const std::string& name) { return vm.getGlobals().at(name); }
};

const char* kSum =
    "fn sum7(n) {\n"
    "    let acc = 0\n"
    "    let i = 0\n"
    "    while i < n {\n"
    "        acc = acc + (i % 7)\n"
    "        i = i + 1\n"
    "    }\n"
    "    return acc\n"
    "}\n"
    "main {\n"
    "}\n";

int expectedSum7(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += i % 7;
    return s;
}

}  // namespace

TEST(ThreadVm, APlainThreadHasNoVm) {
    EXPECT_EQ(currentThreadVM().vm, nullptr);
    EXPECT_EQ(currentThreadVM().loan, nullptr);
    EXPECT_THROW(naab::vm::VM::callToolOnThreadVM(NaabVal::makeNull(), {}, false),
                 std::runtime_error);
}

TEST(ThreadVm, RunningScopesNestAndRestore) {
    int outer = 0, inner = 0;
    {
        RunningVMScope a(&outer);
        EXPECT_EQ(currentThreadVM().vm, &outer);
        {
            RunningVMScope b(&inner);
            EXPECT_EQ(currentThreadVM().vm, &inner);
        }
        EXPECT_EQ(currentThreadVM().vm, &outer);
    }
    EXPECT_EQ(currentThreadVM().vm, nullptr);
}

TEST(ThreadVm, ALoanWithoutAVmLendsNothing) {
    VMLoan loan;
    ThreadVM lent = loan.lent();
    EXPECT_EQ(lent.vm, nullptr);
    EXPECT_EQ(lent.loan, nullptr);
}

TEST(ThreadVm, ALoanIsRevokedWhenTheDispatcherLeaves) {
    int vm = 0;
    ThreadVM lent;
    {
        RunningVMScope running(&vm);
        VMLoan loan;
        lent = loan.lent();
        ASSERT_EQ(lent.vm, &vm);
        ASSERT_NE(lent.loan, nullptr);
        EXPECT_FALSE(lent.loan->revoked.load());
    }
    // The state outlives the loan (the worker still holds it), and says so.
    ASSERT_NE(lent.loan, nullptr);
    EXPECT_TRUE(lent.loan->revoked.load());
}

TEST(ThreadVm, AWorkerSeesTheLentVmAndEnteringItTakesNoFurtherLock) {
    int vm = 0;
    RunningVMScope running(&vm);
    VMLoan loan;
    ThreadVM lent = loan.lent();
    std::atomic<bool> saw_lent{false}, inside_clears_loan{false}, restored{false};
    std::thread worker([&] {
        {
            LentVMScope scope(lent);
            saw_lent = currentThreadVM().vm == &vm && currentThreadVM().loan == lent.loan;
            {
                // What VM::run() does once the worker has entered: a tool the
                // entered code calls is nested and must not wait on the loan's
                // lock, which this worker already holds.
                RunningVMScope entered(&vm);
                inside_clears_loan = currentThreadVM().vm == &vm && !currentThreadVM().loan;
            }
        }
        restored = currentThreadVM().vm == nullptr;
    });
    worker.join();
    EXPECT_TRUE(saw_lent.load());
    EXPECT_TRUE(inside_clears_loan.load());
    EXPECT_TRUE(restored.load());
}

TEST(ThreadVm, ARevokedLoanRefusesEntry) {
    ProgramVM p(kSum);
    NaabVal fn = p.fn("sum7");
    ThreadVM lent;
    std::string before, after;
    int before_value = -1;
    {
        RunningVMScope running(&p.vm);
        VMLoan loan;
        lent = loan.lent();
        std::thread t([&] {
            LentVMScope scope(lent);
            try {
                auto r = naab::vm::VM::callToolOnThreadVM(fn, {NaabVal::makeInt(10)}, false);
                before_value = r.isInt() ? r.asInt() : -2;
            } catch (const std::exception& e) {
                before = e.what();
            }
        });
        t.join();
    }
    // Control: while the loan stood, the entry ran the function.
    EXPECT_EQ(before_value, expectedSum7(10)) << before;
    std::thread t([&] {
        LentVMScope scope(lent);
        try {
            naab::vm::VM::callToolOnThreadVM(fn, {NaabVal::makeInt(10)}, false);
            after = "ran";
        } catch (const std::exception& e) {
            after = e.what();
        }
    });
    t.join();
    EXPECT_NE(after.find("tool not run"), std::string::npos) << after;
}

TEST(ThreadVm, TwoWorkersEnterALentVmOneAtATime) {
    ProgramVM p(kSum);
    NaabVal fn = p.fn("sum7");
    const int n = 200000;
    RunningVMScope running(&p.vm);
    VMLoan loan;
    ThreadVM lent = loan.lent();
    std::vector<int> got(4, -1);
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; w++) {
        workers.emplace_back([&, w] {
            LentVMScope scope(lent);
            try {
                auto r = naab::vm::VM::callToolOnThreadVM(fn, {NaabVal::makeInt(n)}, false);
                got[w] = r.isInt() ? r.asInt() : -2;
            } catch (...) {
                got[w] = -3;
            }
        });
    }
    for (auto& t : workers) t.join();
    for (int w = 0; w < 4; w++) EXPECT_EQ(got[w], expectedSum7(n)) << "worker " << w;
}
