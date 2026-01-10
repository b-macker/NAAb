// NAAb Stack Tracer Implementation
// Cross-language stack trace management (Phase 4.2)

#include "naab/stack_tracer.h"
#include <sstream>

namespace naab {
namespace error {

// Thread-local storage initialization
thread_local std::vector<StackFrame> StackTracer::stack_;

void StackTracer::pushFrame(const StackFrame& frame) {
    stack_.push_back(frame);
}

void StackTracer::popFrame() {
    if (!stack_.empty()) {
        stack_.pop_back();
    }
}

std::vector<StackFrame> StackTracer::getTrace() {
    // Return copy of current stack
    return stack_;
}

void StackTracer::clear() {
    stack_.clear();
}

size_t StackTracer::depth() {
    return stack_.size();
}

std::string StackTracer::formatTrace() {
    if (stack_.empty()) {
        return "<empty stack trace>";
    }

    std::ostringstream ss;
    ss << "Stack trace (most recent call first):\n";

    // Iterate in reverse order (most recent first)
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        ss << it->toString() << "\n";
    }

    return ss.str();
}

} // namespace error
} // namespace naab
