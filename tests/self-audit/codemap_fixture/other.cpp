// codemap fixture: a second translation unit, for includes and cross-file calls.
#include "fixture.h"

void drive(Base& b);
void via_lambda();
void via_value();
void start_thread();
void register_all();
void via_two_lambdas();

thread_local std::string tl_name;   // read through a TLS wrapper from fixture.cpp

// fixture.cpp has a static helper(int) of its own: same name, same parameters
static int helper(int x) { return x * 2; }
void other_helpers() {
    (void)helper(1);
    (void)helper(2);
    (void)helper(3);
}

void registerExecutor(const std::string& language, std::unique_ptr<Base> executor) {
    (void)language;
    executor->run();
}

int main() {
    Leaf l;
    drive(l);
    via_lambda();
    via_value();
    start_thread();
    register_all();
    via_two_lambdas();
    profile_shapes();
    other_helpers();
    return 0;
}
