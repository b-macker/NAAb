// codemap fixture: a second translation unit, for includes and cross-file calls.
#include "fixture.h"

void drive(Base& b);
void via_lambda();
void via_value();
void start_thread();
void register_all();

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
    return 0;
}
