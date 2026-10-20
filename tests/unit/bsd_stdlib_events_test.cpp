// include/naab/bsd_stdlib_events.h against the stdlib modules as compiled.
//
// tests/self-audit/test_bsd_event_coverage.sh reads the modules' SOURCE for
// the missing direction (a function the table does not list); this checks the
// table against the real module objects, and the invariants the VM relies on.

#include <gtest/gtest.h>

#include "naab/bsd_stdlib_events.h"
#include "naab/stdlib.h"

#include <set>
#include <string>

using naab::governance::RuntimeEventType;
using naab::governance::StdlibEventSource;
using naab::governance::preCheckedEventType;
using naab::governance::stdlibEventClass;
using naab::governance::stdlibEventDetail;
namespace bsd_table = naab::governance::bsd_table;

TEST(BsdStdlibEvents, EveryRowNamesAFunctionItsModuleAccepts) {
    naab::stdlib::StdLib lib;
    for (const auto& c : bsd_table::kTable) {
        auto mod = lib.getModule(c.module);
        ASSERT_TRUE(mod) << "no stdlib module '" << c.module << "'";
        EXPECT_TRUE(mod->hasFunction(c.method)) << c.module << "." << c.method
            << " is in the table but the module does not accept it";
    }
}

TEST(BsdStdlibEvents, EveryCoveredModuleHasRowsAndEveryRowIsCovered) {
    std::set<std::string> covered(std::begin(naab::governance::kCoveredModules),
                                  std::end(naab::governance::kCoveredModules));
    std::set<std::string> with_rows;
    for (const auto& c : bsd_table::kTable) {
        with_rows.insert(c.module);
        EXPECT_TRUE(covered.count(c.module)) << c.module << " has rows but is not covered";
    }
    for (const auto& m : covered) EXPECT_TRUE(with_rows.count(m)) << m << " is covered but has no rows";
}

// The VM pre-checks a Module-recorded call and leaves its record to the module:
// the pre-check's consumed mark is then cleared by the module's own emit of the
// SAME type on the same thread. Only agentSend() does that (AGENT_SEND before
// its retry loop), so no other row may be Module-recorded.
TEST(BsdStdlibEvents, OnlyAgentSendIsRecordedByItsModule) {
    for (const auto& c : bsd_table::kTable) {
        if (c.source != StdlibEventSource::Module) continue;
        EXPECT_EQ(std::string(c.module) + "." + c.method, "agent.send");
        EXPECT_EQ(c.type, RuntimeEventType::AGENT_SEND);
        EXPECT_TRUE(preCheckedEventType(c.type));
        EXPECT_FALSE(c.arg_in_detail) << "agent.send's first argument is the handle (its __nonce)";
    }
}

// Two names for one code branch must be one class (A22: env.get_all was unseen
// while env.list was recorded, env.set while env.set_var was).
TEST(BsdStdlibEvents, AliasesShareTheirCanonicalClass) {
    const std::pair<const char*, const char*> aliases[][2] = {
        {{"env", "get_all"}, {"env", "list"}},
        {{"env", "set"}, {"env", "set_var"}},
    };
    for (const auto& a : aliases) {
        auto x = stdlibEventClass(a[0].first, a[0].second);
        auto y = stdlibEventClass(a[1].first, a[1].second);
        ASSERT_TRUE(x && y);
        EXPECT_EQ(x->source, StdlibEventSource::Vm);
        EXPECT_EQ(x->source, y->source);
        EXPECT_EQ(x->type, y->type) << a[0].first << "." << a[0].second;
    }
}

TEST(BsdStdlibEvents, PreCheckedTypesAreUnchanged) {
    for (auto t : {RuntimeEventType::ENV_READ, RuntimeEventType::FILE_READ, RuntimeEventType::FILE_WRITE,
                   RuntimeEventType::NET_CONNECT, RuntimeEventType::ENCODE,
                   RuntimeEventType::PROCESS_EXEC, RuntimeEventType::AGENT_SEND})
        EXPECT_TRUE(preCheckedEventType(t));
    EXPECT_FALSE(preCheckedEventType(RuntimeEventType::ENV_WRITE));
    EXPECT_FALSE(preCheckedEventType(RuntimeEventType::DECODE));
}

TEST(BsdStdlibEvents, UnlistedNamesHaveNoClass) {
    EXPECT_EQ(stdlibEventClass("string", "upper"), nullptr);   // a module the table does not cover
    EXPECT_EQ(stdlibEventClass("env", "getenv"), nullptr);     // an error branch, not a function
    auto has = stdlibEventClass("env", "has");
    ASSERT_TRUE(has);
    EXPECT_EQ(has->source, StdlibEventSource::None);
}

TEST(BsdStdlibEvents, DetailCarriesTheFirstArgumentAtMost64Chars) {
    auto r = stdlibEventClass("file", "read");
    ASSERT_TRUE(r);
    EXPECT_EQ(stdlibEventDetail(*r, "file.read", "in.txt"), "file.read('in.txt')");
    EXPECT_EQ(stdlibEventDetail(*r, "file.read", ""), "file.read");
    EXPECT_EQ(stdlibEventDetail(*r, "file.read", std::string(100, 'x')),
              "file.read('" + std::string(64, 'x') + "')");
    auto s = stdlibEventClass("agent", "send");
    ASSERT_TRUE(s);
    EXPECT_EQ(stdlibEventDetail(*s, "agent.send", "{__nonce: abc}"), "agent.send");
}
