// codemap fixture: see include/fixture.h.
#include "fixture.h"

void sink(const std::string& path) { (void)path; }
bool gate(const std::string& path) { return !path.empty(); }

// reach: two entry points nothing calls -- one reaches sink only through the
// gate, the other without it
void gated_caller(const std::string& p) {
    if (gate(p)) sink(p);
}
void ungated_caller(const std::string& p) { sink(p); }
void entry_gated() { gated_caller("g"); }
void entry_ungated() { ungated_caller("u"); }

// a call made inside a lambda belongs to the lambda, which the function defines
void via_lambda() {
    auto f = [](const std::string& s) { sink(s); };
    f("l");
}

// a function taken as a value: called through std::function, so no call site names it
void via_value() {
    std::function<void(const std::string&)> cb = sink;
    cb("v");
}

// virtual dispatch: drive() calls Base::run, which lands in Derived::run or Leaf::run
void Derived::run() { sink("d"); }
void Leaf::run() { sink("leaf"); }
void drive(Base& b) { b.run(); }

// a thread start
void worker() { sink("t"); }
void start_thread() {
    std::thread t([] { worker(); });
    t.join();
}

// NAAb-name dispatch
int read_impl(int x) { return x + 1; }
int write_impl(int x) { return x + 2; }
int unknown_impl(int x) { return -x; }
int FileModule::call(const std::string& function_name, int x) {
    if (function_name == "read") {
        return read_impl(x);
    } else if (function_name == "write") {
        return write_impl(x);
    } else {
        return unknown_impl(x);   // no NAAb name reaches this branch
    }
}

void register_all() { registerExecutor("fixlang", std::make_unique<Derived>()); }

// TODO(codemap): fixture task tag -- tags must find this line
// FIXME(codemap): non-ASCII — café — must reach the terminal as ASCII
// Record reference for the tag scanner: see A45 in docs/open-investigations.md.
void dead_function() { (void)0; }

#ifdef _WIN32
// Excluded by the preprocessor on this build: the index must not see it.
void windows_only() { sink("w"); }
#endif

// A4: a test-arm label, not a citation -- the tag scanner must not report it
// (the citation two blocks up must be reported).

// --- a lambda stored in a named variable is findable by that name
void via_named_lambda() {
    auto named_cb = [](const std::string& s) { sink(s); };
    named_cb("n");
}

// --- writes
static Budget g_budget;
void charge() {
    g_budget.calls++;
    g_budget.tokens += 5;
    g_budget.log.push_back(1);
}
void reset_budget() { g_budget.calls = 0; }
int peek_budget() { return g_budget.calls; }
void local_only() {
    int local = 0;
    local = 3;
    (void)local;
}

// --- the pre-dispatch allowlist: fixfile.read is gated before dispatch,
// fixfile.write is not
std::string GovernanceEngine::filesystemAccessMode(const std::string& module,
                                                   const std::string& method) {
    if (module == "fixfile") {
        if (method == "read") return "read";
        return "";
    }
    return "";
}
bool GovernanceEngine::checkFilesystemAllowed(const std::string& mode) { return !mode.empty(); }
bool GovernanceEngine::checkPathAccess(const std::string& path) { return !path.empty(); }
int vm_dispatch(GovernanceEngine& g, Module& m, const std::string& module,
                const std::string& method, int x) {
    std::string mode = g.filesystemAccessMode(module, method);
    if (!mode.empty()) {
        g.checkFilesystemAllowed(mode);
        g.checkPathAccess(method);
    }
    return m.call(method, x);
}
