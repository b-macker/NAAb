// NAAb Stack Tracer Implementation
// Cross-language stack trace management (Phase 4.2)

#include "naab/stack_tracer.h"
#include "naab/stack_formatter.h"  // Phase 4.2.6: Enhanced formatting
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

    // Phase 4.2.6: Use enhanced formatter with color support
    // TODO: Check global color setting from Diagnostic::isGlobalColorEnabled()
    // For now, use colored output by default
    return StackFormatter::formatColored(stack_);
}

} // namespace error
} // namespace naab
