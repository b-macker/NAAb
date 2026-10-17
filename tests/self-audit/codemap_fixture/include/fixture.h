// codemap fixture: one instance of every edge kind the map claims to record.
// tests/self-audit/test_codemap.sh indexes this tree and asserts each edge,
// each with a control; tests/self-audit/test_codemap_profile.sh also builds it
// and runs it under callgrind, so every shape below must compile and run.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <thread>

void sink(const std::string& path);   // the leaf a reviewer searches back from
bool gate(const std::string& path);   // the check that should precede it

class Base {
public:
    virtual ~Base() = default;
    virtual void run() = 0;
};

class Derived : public Base {
public:
    void run() override;
};

class Leaf : public Derived {
public:
    void run() override;
};

// The stdlib dispatch shape: getName() names the module, call() branches on
// the function name a NAAb program wrote.
class Module {
public:
    virtual ~Module() = default;
    virtual std::string getName() const = 0;
    virtual int call(const std::string& function_name, int x) = 0;
};

class FileModule : public Module {
public:
    std::string getName() const override { return "fixfile"; }
    int call(const std::string& function_name, int x) override;
};

int read_impl(int x);
int write_impl(int x);
int unknown_impl(int x);

// The executor registration shape: a language name mapped to a class.
void registerExecutor(const std::string& language, std::unique_ptr<Base> executor);

// --- writes: fields assigned, mutated, and only read --------------------------
#include <atomic>
#include <vector>
struct Budget {
    int calls = 0;
    std::atomic<int> tokens{0};
    std::vector<int> log;
};
void charge();
void reset_budget();
int peek_budget();
void local_only();

// --- the pre-dispatch shape: an allowlist consulted, then the gates, then dispatch
class GovernanceEngine {
public:
    std::string filesystemAccessMode(const std::string& module, const std::string& method);
    bool checkFilesystemAllowed(const std::string& mode);
    bool checkPathAccess(const std::string& path);
};
int vm_dispatch(GovernanceEngine& g, Module& m, const std::string& module,
                const std::string& method, int x);

// --- profile matching: frames a name alone cannot place ---------------------
void over(int x);                        // overloads share a name: called 2x, 1x, never
void over(const std::string& s);
void over(double d);
struct Pt {                              // a user constructor beside the implicit copy
    Pt(int a, int b);
    int x;
    int y;
    std::string tag;                     // makes the copy a real call
};
struct Box {                             // const and non-const overloads
    int v = 0;
    int get() const;
    int& get();
};
std::string make_label(int n);           // GCC tags it make_label[abi:cxx11]
using Label = std::string;
void takes_alias(Label l);               // written as an alias, demangled canonically
struct Left {
    virtual ~Left() = default;
    virtual int l() { return 1; }
};
struct Right {
    virtual ~Right() = default;
    virtual int r() const = 0;
};
struct Pair : Left, Right {              // r() through Right& goes through a thunk
    int r() const override;
};
template <class T> int pick_t(T v) { return static_cast<int>(sizeof(v)); }
template <class T> int pick_t(T* p) { return p ? 1 : 0; }   // same name, same arity
void hold(std::shared_ptr<int> p);       // overloads told apart only by a template argument
void hold(std::shared_ptr<double> p);
template <class... A> int count_args(const char* f, A&&... a) {   // a parameter pack
    return static_cast<int>(sizeof...(a)) + (f ? 0 : 1);
}
void other_helpers();                    // other.cpp's own static helper(int)
void refuse(const std::string& why);     // the path a HARD block takes before _exit(3)
int one_liner();                         // a function and its lambda on ONE line
int twin_a();                            // two definitions on one line: no position
int twin_b();                            //   tells them apart
int never_linked(int x);                 // defined in extra.cpp, which no binary links
void touch_tls();
void profile_shapes();
