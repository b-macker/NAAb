#include "naab/debugger.h"
#include "naab/ast.h"
#include <sstream>
#include <algorithm>
#include <iostream>

namespace naab {
namespace debugger {

// Helper function to format source location
std::string formatSourceLocation(const std::string& filename, int line, int column) {
    std::ostringstream oss;
    oss << filename << ":" << line << ":" << column;
    return oss.str();
}

// Debugger constructor
Debugger::Debugger()
    : next_breakpoint_id_(1),
      next_watch_id_(1),
      paused_(false),
      active_(true),
      current_step_mode_(StepMode::NONE),
      step_frame_depth_(0),
      current_environment_(nullptr),
      breakpoint_callback_(nullptr) {
}

Debugger::~Debugger() {
}

// Breakpoint management
int Debugger::setBreakpoint(const std::string& location, const std::string& condition) {
    int id = next_breakpoint_id_++;
    Breakpoint bp(id, location, condition);
    breakpoints_[id] = bp;
    return id;
}

bool Debugger::clearBreakpoint(int id) {
    auto it = breakpoints_.find(id);
    if (it != breakpoints_.end()) {
        breakpoints_.erase(it);
        return true;
    }
    return false;
}

bool Debugger::enableBreakpoint(int id) {
    auto it = breakpoints_.find(id);
    if (it != breakpoints_.end()) {
        it->second.enabled = true;
        return true;
    }
    return false;
}

bool Debugger::disableBreakpoint(int id) {
    auto it = breakpoints_.find(id);
    if (it != breakpoints_.end()) {
        it->second.enabled = false;
        return true;
    }
    return false;
}

std::vector<Breakpoint> Debugger::listBreakpoints() const {
    std::vector<Breakpoint> result;
    for (const auto& pair : breakpoints_) {
        result.push_back(pair.second);
    }
    return result;
}

Breakpoint* Debugger::getBreakpoint(int id) {
    auto it = breakpoints_.find(id);
    if (it != breakpoints_.end()) {
        return &it->second;
    }
    return nullptr;
}

// Execution control
void Debugger::step(StepMode mode) {
    current_step_mode_ = mode;

    // Set the step frame depth to current depth for OVER and OUT modes
    if (mode == StepMode::OVER || mode == StepMode::OUT) {
        step_frame_depth_ = getCurrentDepth();
    }

    // Resume execution
    paused_ = false;
}

void Debugger::resume() {
    current_step_mode_ = StepMode::CONTINUE;
    paused_ = false;
}

void Debugger::pause() {
    paused_ = true;
}

bool Debugger::isPaused() const {
    return paused_;
}

bool Debugger::shouldBreak(const ast::Node& node) {
    if (!active_) {
        return false;
    }

    std::string location = formatLocation(node);
    return shouldBreak(location);
}

bool Debugger::shouldBreak(const std::string& location) {
    if (!active_) {
        return false;
    }

    // Check step mode
    int current_depth = getCurrentDepth();

    switch (current_step_mode_) {
        case StepMode::INTO:
            // Stop at any statement
            paused_ = true;
            current_step_mode_ = StepMode::NONE;
            return true;

        case StepMode::OVER:
            // Stop when we return to same or higher level
            if (current_depth <= step_frame_depth_) {
                paused_ = true;
                current_step_mode_ = StepMode::NONE;
                return true;
            }
            break;

        case StepMode::OUT:
            // Stop when we return to parent level
            if (current_depth < step_frame_depth_) {
                paused_ = true;
                current_step_mode_ = StepMode::NONE;
                return true;
            }
            break;

        case StepMode::NONE:
        case StepMode::CONTINUE:
            // Only break on breakpoints
            break;
    }

    // Check breakpoints
    for (auto& pair : breakpoints_) {
        Breakpoint& bp = pair.second;

        if (!bp.enabled) {
            continue;
        }

        if (matchesLocation(location, bp.location)) {
            // Check condition if present
            if (!bp.condition.empty()) {
                if (!evaluateCondition(bp.condition)) {
                    continue;
                }
            }

            // Hit the breakpoint
            bp.hit_count++;
            paused_ = true;

            // Call callback if set
            if (breakpoint_callback_ && !call_stack_.empty()) {
                breakpoint_callback_(bp, call_stack_.back());
            }

            return true;
        }
    }

    return false;
}

StepMode Debugger::getCurrentStepMode() const {
    return current_step_mode_;
}

void Debugger::setCurrentStepMode(StepMode mode) {
    current_step_mode_ = mode;
}

int Debugger::getStepFrameDepth() const {
    return step_frame_depth_;
}

void Debugger::setStepFrameDepth(int depth) {
    step_frame_depth_ = depth;
}

// Call stack management
void Debugger::pushFrame(const CallFrame& frame) {
    call_stack_.push_back(frame);
}

void Debugger::popFrame() {
    if (!call_stack_.empty()) {
        call_stack_.pop_back();
    }
}

std::vector<CallFrame> Debugger::getCallStack() const {
    return call_stack_;
}

CallFrame* Debugger::getCurrentFrame() {
    if (call_stack_.empty()) {
        return nullptr;
    }
    return &call_stack_.back();
}

int Debugger::getCurrentDepth() const {
    return static_cast<int>(call_stack_.size());
}

// Variable inspection
std::shared_ptr<interpreter::Value> Debugger::inspectVariable(const std::string& name) {
    if (!current_environment_) {
        return nullptr;
    }

    // Try to get variable from current environment
    // Note: This requires access to Environment::get() method
    // Implementation depends on Environment class interface
    // For now, return nullptr - will be implemented when integrating with interpreter
    return nullptr;
}

std::map<std::string, std::shared_ptr<interpreter::Value>> Debugger::listLocalVariables() {
    std::map<std::string, std::shared_ptr<interpreter::Value>> result;

    if (!call_stack_.empty()) {
        const CallFrame& frame = call_stack_.back();
        result = frame.locals;
    }

    return result;
}

std::map<std::string, std::shared_ptr<interpreter::Value>> Debugger::listGlobalVariables() {
    std::map<std::string, std::shared_ptr<interpreter::Value>> result;

    // Implementation depends on Environment class interface
    // Will be implemented when integrating with interpreter

    return result;
}

// Watch expressions
int Debugger::addWatch(const std::string& expression) {
    int id = next_watch_id_++;
    watches_[id] = expression;
    return id;
}

bool Debugger::removeWatch(int id) {
    auto it = watches_.find(id);
    if (it != watches_.end()) {
        watches_.erase(it);
        return true;
    }
    return false;
}

std::vector<WatchResult> Debugger::evaluateWatches() {
    std::vector<WatchResult> results;

    for (const auto& pair : watches_) {
        WatchResult result;
        result.id = pair.first;
        result.expression = pair.second;

        // Evaluate the watch expression
        // Implementation depends on having access to interpreter
        // For now, set as error - will be implemented when integrating
        result.error = "Watch evaluation not yet implemented";

        results.push_back(result);
    }

    return results;
}

std::vector<std::string> Debugger::listWatches() const {
    std::vector<std::string> result;
    for (const auto& pair : watches_) {
        result.push_back(pair.second);
    }
    return result;
}

// State management
void Debugger::reset() {
    breakpoints_.clear();
    watches_.clear();
    call_stack_.clear();
    paused_ = false;
    current_step_mode_ = StepMode::NONE;
    step_frame_depth_ = 0;
    next_breakpoint_id_ = 1;
    next_watch_id_ = 1;
}

bool Debugger::isActive() const {
    return active_;
}

void Debugger::setActive(bool active) {
    active_ = active;
}

// Environment access
void Debugger::setCurrentEnvironment(std::shared_ptr<interpreter::Environment> env) {
    current_environment_ = env;
}

std::shared_ptr<interpreter::Environment> Debugger::getCurrentEnvironment() const {
    return current_environment_;
}

// Breakpoint hit callback
void Debugger::setBreakpointCallback(BreakpointCallback callback) {
    breakpoint_callback_ = callback;
}

// Helper methods
bool Debugger::evaluateCondition(const std::string& condition) {
    // Placeholder: condition evaluation requires interpreter integration
    // For now, always return true
    // TODO: Implement condition evaluation using interpreter
    return true;
}

bool Debugger::matchesLocation(const std::string& location, const std::string& breakpoint_location) {
    // Exact match
    if (location == breakpoint_location) {
        return true;
    }

    // Check if breakpoint is just a function name
    // Location format: "filename:line:col" or "function_name"
    // Breakpoint format: "filename:line" or "function_name"

    // Extract function name from location if present
    size_t colon_pos = location.find(':');
    if (colon_pos == std::string::npos) {
        // Location is just a function name
        return location == breakpoint_location;
    }

    // Check if breakpoint matches file:line
    std::string file_line = location.substr(0, location.rfind(':'));
    if (file_line == breakpoint_location) {
        return true;
    }

    // Check just filename:line without column
    size_t second_colon = location.find(':', colon_pos + 1);
    if (second_colon != std::string::npos) {
        std::string file_line_only = location.substr(0, second_colon);
        if (file_line_only == breakpoint_location) {
            return true;
        }
    }

    return false;
}

std::string Debugger::formatLocation(const ast::Node& node) {
    // Get location from AST node
    // This depends on AST node having location information
    // For now, return a placeholder
    // TODO: Implement using actual AST location data
    return "unknown:0:0";
}

} // namespace debugger
} // namespace naab
