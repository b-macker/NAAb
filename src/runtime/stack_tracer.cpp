// NAAb Stack Tracer Implementation
// Cross-language stack trace management (Phase 4.2)

#include "naab/stack_tracer.h"
#include "naab/stack_formatter.h"  // Phase 4.2.6: Enhanced formatting
#include "naab/error_reporter.h"   // Diagnostic::isGlobalColorEnabled()
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

    // Phase 4.2.6: Use enhanced formatter, honoring the global color setting.
    // Traces are interpolated into exception messages that land in logs/pipes,
    // so respect --no-color and non-TTY output rather than always emitting ANSI.
    return Diagnostic::isGlobalColorEnabled()
        ? StackFormatter::formatColored(stack_)
        : StackFormatter::formatPlain(stack_);
}

} // namespace error
} // namespace naab
