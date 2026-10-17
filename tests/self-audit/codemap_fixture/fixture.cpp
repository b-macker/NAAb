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

// --- GCC numbers lambdas per SIGNATURE: b is `{lambda(std::string const&)#1}`
// although it is the third lambda in the function, and c, by value, is
// `{lambda(std::string)#1}` -- a different signature, numbered separately
void via_two_lambdas() {
    auto a = [](int x) { return x + 1; };
    auto c = [](std::string s) { return s.size(); };
    auto b = [](const std::string& s) { sink(s); };
    b("t");
    (void)a(1);
    (void)c("u");
}

// --- profile matching: each shape needs more than the frame's name to place it
void over(int x) { (void)x; }
void over(const std::string& s) { (void)s; }
void over(double d) { (void)d; }                    // never called
Pt::Pt(int a, int b) : x(a), y(b), tag("pt") {}
int Box::get() const { return v; }
int& Box::get() { return v; }
std::string make_label(int n) { return std::string(static_cast<size_t>(n), 'x'); }
void takes_alias(Label l) { (void)l; }
int Pair::r() const { return 2; }
extern thread_local std::string tl_name;            // defined in other.cpp
static int helper(int x) { return x + 2; }          // other.cpp has a static helper(int) too
void hold(std::shared_ptr<int> p) { (void)p; }
void refuse(const std::string& why) { sink(why); }
int one_liner() { return [](int v) { return v + 1; }(1); }
int twin_a() { return 1; } int twin_b() { return 2; }
void hold(std::shared_ptr<double> p) { (void)p; }
void touch_tls() { tl_name = "tls"; }

// a lambda inside a lambda is numbered within its owner; the second top-level
// lambda is #2 of this function however many lambdas the first one holds
void nested_lambdas() {
    auto outer = [] {
        auto inner = [] { (void)0; };
        inner();
    };
    auto second = [] { (void)0; };
    outer();
    second();
    second();
}

// a generic lambda's operator() is a template: operator()<int>
void generic_lambda() {
    auto g = [](auto&& v) { return v + 1; };
    (void)g(1);
    (void)g(2);
}

void profile_shapes() {
    over(1);
    over(2);
    over(std::string("o"));
    Pt p(1, 2);
    Pt q = p;
    Pt r = q;
    (void)r;
    Box box;
    const Box& cbox = box;
    (void)cbox.get();
    box.get() = 1;
    box.get() = 2;
    box.get() = 3;
    (void)make_label(2);
    (void)make_label(3);
    takes_alias("alias");
    Pair pair;
    const Right& right = pair;
    (void)right.r();
    touch_tls();
    nested_lambdas();
    generic_lambda();
    (void)pick_t(1);
    (void)helper(1);
    (void)helper(2);
    hold(std::make_shared<int>(1));
    hold(std::make_shared<double>(1.0));
    hold(std::make_shared<double>(2.0));
    (void)count_args("x");
    (void)count_args("y", 1, 2);
    (void)one_liner();
    (void)twin_a();
}
