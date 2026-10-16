// codemap fixture: one instance of every edge kind the map claims to record.
// tests/self-audit/test_codemap.sh indexes this tree and asserts each edge,
// each with a control. Nothing here is built or run; it is only parsed.
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
