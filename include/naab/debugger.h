#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include "ast.h"

namespace naab {
namespace ast {
    class Node;
    class Stmt;
}

namespace interpreter {
    class Value;
    class Environment;
}

namespace debugger {

// Step execution modes
enum class StepMode {
    NONE,       // Not stepping, run until breakpoint or completion
    OVER,       // Step over: continue until same frame level
    INTO,       // Step into: stop at next statement (any depth)
    OUT,        // Step out: continue until parent frame returns
    CONTINUE    // Continue execution until next breakpoint
};

// Breakpoint structure
struct Breakpoint {
    int id;
    std::string location;      // "file:line" or "function_name"
    std::string condition;     // Optional conditional expression
    int hit_count;             // Number of times this breakpoint has been hit
    bool enabled;              // Whether this breakpoint is active

    Breakpoint()
        : id(0), hit_count(0), enabled(true) {}

    Breakpoint(int id, const std::string& location, const std::string& condition = "")
        : id(id), location(location), condition(condition), hit_count(0), enabled(true) {}
};

// Watch expression result
struct WatchResult {
    int id;
    std::string expression;
    std::shared_ptr<interpreter::Value> value;
    std::string error;  // Empty if successful, error message otherwise
};

// Call frame for stack inspection
struct CallFrame {
    std::string function_name;
    std::string source_location;  // file:line:col format
    std::map<std::string, std::shared_ptr<interpreter::Value>> locals;
    std::shared_ptr<interpreter::Environment> env;
    int frame_depth;              // Depth in call stack (0 = top level)

    CallFrame()
        : frame_depth(0) {}

    CallFrame(const std::string& name, const std::string& location, int depth)
        : function_name(name), source_location(location), frame_depth(depth) {}
};

// Main Debugger class
class Debugger {
public:
    Debugger();
    ~Debugger();

    // Breakpoint management
    int setBreakpoint(const std::string& location, const std::string& condition = "");
    bool clearBreakpoint(int id);
    bool enableBreakpoint(int id);
    bool disableBreakpoint(int id);
    std::vector<Breakpoint> listBreakpoints() const;
    Breakpoint* getBreakpoint(int id);

    // Execution control
    void step(StepMode mode);
    void resume();
    void pause();
    bool isPaused() const;
    bool shouldBreak(const ast::Node& node);
    bool shouldBreak(const std::string& location);

    // Step mode management
    StepMode getCurrentStepMode() const;
    void setCurrentStepMode(StepMode mode);
    int getStepFrameDepth() const;
    void setStepFrameDepth(int depth);

    // Call stack management
    void pushFrame(const CallFrame& frame);
    void popFrame();
    std::vector<CallFrame> getCallStack() const;
    CallFrame* getCurrentFrame();
    int getCurrentDepth() const;

    // Variable inspection
    std::shared_ptr<interpreter::Value> inspectVariable(const std::string& name);
    std::map<std::string, std::shared_ptr<interpreter::Value>> listLocalVariables();
    std::map<std::string, std::shared_ptr<interpreter::Value>> listGlobalVariables();

    // Watch expressions
    int addWatch(const std::string& expression);
    bool removeWatch(int id);
    std::vector<WatchResult> evaluateWatches();
    std::vector<std::string> listWatches() const;

    // State management
    void reset();
    bool isActive() const;
    void setActive(bool active);

    // Environment access (for variable inspection and watch evaluation)
    void setCurrentEnvironment(std::shared_ptr<interpreter::Environment> env);
    std::shared_ptr<interpreter::Environment> getCurrentEnvironment() const;

    // Breakpoint hit callback
    using BreakpointCallback = std::function<void(const Breakpoint&, const CallFrame&)>;
    void setBreakpointCallback(BreakpointCallback callback);

private:
    // Breakpoint storage
    std::map<int, Breakpoint> breakpoints_;
    int next_breakpoint_id_;

    // Watch storage
    std::map<int, std::string> watches_;
    int next_watch_id_;

    // Call stack
    std::vector<CallFrame> call_stack_;

    // Execution state
    bool paused_;
    bool active_;
    StepMode current_step_mode_;
    int step_frame_depth_;

    // Current environment for variable inspection
    std::shared_ptr<interpreter::Environment> current_environment_;

    // Callback for breakpoint hits
    BreakpointCallback breakpoint_callback_;

    // Helper methods
    bool evaluateCondition(const std::string& condition);
    bool matchesLocation(const std::string& location, const std::string& breakpoint_location);
    std::string formatLocation(const ast::Node& node);
};

// Helper function to format source location
std::string formatSourceLocation(const std::string& filename, int line, int column);

} // namespace debugger
} // namespace naab
