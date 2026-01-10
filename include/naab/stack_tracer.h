#ifndef NAAB_STACK_TRACER_H
#define NAAB_STACK_TRACER_H

#include "naab/stack_frame.h"
#include <vector>
#include <string>

namespace naab {
namespace error {

/**
 * StackTracer: Thread-local stack trace management (Phase 4.2.1)
 *
 * Maintains a call stack across language boundaries for unified error reporting.
 * Uses thread-local storage to support multi-threaded execution.
 */
class StackTracer {
public:
    /**
     * Push a new frame onto the call stack
     *
     * @param frame Stack frame to push
     */
    static void pushFrame(const StackFrame& frame);

    /**
     * Pop the top frame from the call stack
     *
     * Should be called when returning from a function
     */
    static void popFrame();

    /**
     * Get the current stack trace
     *
     * @return Vector of stack frames from most recent (top) to oldest (bottom)
     */
    static std::vector<StackFrame> getTrace();

    /**
     * Clear the entire stack trace
     *
     * Useful for error recovery or test cleanup
     */
    static void clear();

    /**
     * Get the current stack depth
     *
     * @return Number of frames currently on the stack
     */
    static size_t depth();

    /**
     * Format stack trace as multi-line string
     *
     * @return Formatted stack trace ready for display
     */
    static std::string formatTrace();

private:
    // Thread-local storage for the call stack
    static thread_local std::vector<StackFrame> stack_;
};

/**
 * ScopedStackFrame: RAII helper for automatic push/pop (Phase 4.2.1)
 *
 * Automatically pushes a frame on construction and pops it on destruction.
 * Ensures stack integrity even when exceptions are thrown.
 *
 * Usage:
 *   void myFunction() {
 *       ScopedStackFrame frame("naab", "myFunction", "main.naab", 42);
 *       // ... function body ...
 *       // Frame automatically popped on return or exception
 *   }
 */
class ScopedStackFrame {
public:
    ScopedStackFrame(const std::string& language,
                     const std::string& function_name,
                     const std::string& filename = "",
                     size_t line_number = 0)
        : frame_(language, function_name, filename, line_number) {
        StackTracer::pushFrame(frame_);
    }

    ScopedStackFrame(const StackFrame& frame)
        : frame_(frame) {
        StackTracer::pushFrame(frame_);
    }

    ~ScopedStackFrame() {
        StackTracer::popFrame();
    }

    // Disable copying to prevent double-pop
    ScopedStackFrame(const ScopedStackFrame&) = delete;
    ScopedStackFrame& operator=(const ScopedStackFrame&) = delete;

private:
    StackFrame frame_;
};

} // namespace error
} // namespace naab

#endif // NAAB_STACK_TRACER_H
