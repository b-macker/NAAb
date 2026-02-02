# Phase 4.4: Debugger (naab-debug) - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** HIGH - Requires interpreter instrumentation and DAP protocol
**Estimated Effort:** 4-5 weeks implementation
**Priority:** HIGH - Essential for debugging complex applications

This document outlines the design for `naab-debug`, an interactive debugger for NAAb with breakpoints, stepping, variable inspection, and IDE integration via Debug Adapter Protocol (DAP).

---

## Current Problem

**No Debugging Support:**
- Cannot set breakpoints
- Cannot step through code line by line
- Cannot inspect variable values during execution
- Must use `print()` statements for debugging (tedious, inefficient)
- No integration with VS Code or other IDEs

**Impact:** Difficult to debug complex applications, slow development, frustrating developer experience.

---

## Goals

### Primary Goals

1. **Breakpoints** - Pause execution at specific lines or functions
2. **Stepping** - Step over, into, out of functions
3. **Variable Inspection** - View variable values at any point
4. **Stack Traces** - View call stack and navigate frames
5. **IDE Integration** - Debug in VS Code via DAP

### Secondary Goals

6. **Conditional Breakpoints** - Break only when condition is true
7. **Watch Expressions** - Evaluate expressions while debugging
8. **Hot Reload** - Modify code during debugging (future)

---

## Architecture

### Components

```
┌──────────────────────────────────────────────┐
│            VS Code / IDE                      │
│                                               │
│  ┌──────────────────────────────────────┐   │
│  │     Debug UI (Breakpoints, Stack,    │   │
│  │     Variables, Watch)                │   │
│  └────────────────┬─────────────────────┘   │
│                   │ DAP (JSON-RPC)           │
└───────────────────┼──────────────────────────┘
                    │
        ┌───────────▼──────────────┐
        │   naab-debug             │
        │   (Debug Adapter)        │
        │                          │
        │  - Protocol handling     │
        │  - Breakpoint management │
        │  - Execution control     │
        └───────────┬──────────────┘
                    │
        ┌───────────▼──────────────┐
        │   NAAb Interpreter       │
        │   (Instrumented)         │
        │                          │
        │  - Breakpoint checks     │
        │  - Step mode             │
        │  - Variable access       │
        └──────────────────────────┘
```

### Project Structure

```
tools/naab-debug/
├── main.cpp                    # CLI debugger entry point
├── debugger.h/cpp             # Core debugger
├── dap_server.h/cpp           # Debug Adapter Protocol server
├── breakpoint_manager.h/cpp   # Breakpoint management
├── stack_manager.h/cpp        # Call stack tracking
└── protocol/
    ├── dap_protocol.h/cpp     # DAP message types
    └── requests.h/cpp         # DAP request handlers
```

---

## Design: Debugger Core

### Debugger Class

```cpp
class Debugger {
public:
    Debugger(Interpreter* interpreter);

    // Breakpoints
    void setBreakpoint(const std::string& file, int line);
    void setFunctionBreakpoint(const std::string& function_name);
    void setConditionalBreakpoint(const std::string& file, int line, const std::string& condition);
    void removeBreakpoint(int breakpoint_id);
    void toggleBreakpoint(int breakpoint_id);

    // Execution control
    void run();                 // Start/continue execution
    void pause();              // Pause execution
    void stepOver();           // Execute next line, don't enter functions
    void stepInto();           // Execute next line, enter functions
    void stepOut();            // Execute until current function returns
    void continue_();          // Resume execution until next breakpoint

    // Variable inspection
    Value inspectVariable(const std::string& name);
    std::vector<Variable> getLocalVariables();
    std::vector<Variable> getGlobalVariables();

    // Stack trace
    std::vector<StackFrame> getStackTrace();
    void selectFrame(int frame_id);

    // Watch expressions
    void addWatchExpression(const std::string& expression);
    std::vector<WatchResult> evaluateWatches();

    // State
    bool isRunning() const;
    bool isPaused() const;
    SourceLocation getCurrentLocation() const;

private:
    Interpreter* interpreter_;
    BreakpointManager breakpoint_manager_;
    StackManager stack_manager_;
    DebugState state_;

    void onBeforeStatement(const Stmt* stmt);
    void onAfterStatement(const Stmt* stmt);
    void onFunctionEnter(const FunctionDecl* func, const std::vector<Value>& args);
    void onFunctionExit(const FunctionDecl* func, const Value& return_value);
};

enum class DebugState {
    Running,
    Paused,
    SteppingOver,
    SteppingInto,
    SteppingOut,
    Stopped
};

struct Variable {
    std::string name;
    Type type;
    Value value;
    int variablesReference;  // For complex types (struct, list, dict)
};

struct StackFrame {
    int id;
    std::string name;              // Function name
    SourceLocation location;       // File, line, column
    std::map<std::string, Value> locals;
};
```

---

## Breakpoint System

### Breakpoint Types

**1. Line Breakpoint**
```cpp
struct LineBreakpoint {
    int id;
    std::string file;
    int line;
    bool enabled;
};
```

**2. Function Breakpoint**
```cpp
struct FunctionBreakpoint {
    int id;
    std::string function_name;
    bool enabled;
};
```

**3. Conditional Breakpoint**
```cpp
struct ConditionalBreakpoint {
    int id;
    std::string file;
    int line;
    std::string condition;  // NAAb expression
    bool enabled;
};
```

### Breakpoint Manager

```cpp
class BreakpointManager {
public:
    int addLineBreakpoint(const std::string& file, int line);
    int addFunctionBreakpoint(const std::string& function_name);
    int addConditionalBreakpoint(const std::string& file, int line, const std::string& condition);

    void removeBreakpoint(int id);
    void toggleBreakpoint(int id);

    bool shouldBreak(const SourceLocation& location);
    bool shouldBreakOnFunction(const std::string& function_name);

private:
    std::vector<LineBreakpoint> line_breakpoints_;
    std::vector<FunctionBreakpoint> function_breakpoints_;
    std::vector<ConditionalBreakpoint> conditional_breakpoints_;
    int next_id_ = 1;

    bool evaluateCondition(const std::string& condition, const Environment& env);
};
```

**Breakpoint Checking:**

```cpp
bool BreakpointManager::shouldBreak(const SourceLocation& location) {
    // Check line breakpoints
    for (const auto& bp : line_breakpoints_) {
        if (bp.enabled && bp.file == location.file && bp.line == location.line) {
            return true;
        }
    }

    // Check conditional breakpoints
    for (const auto& bp : conditional_breakpoints_) {
        if (bp.enabled && bp.file == location.file && bp.line == location.line) {
            if (evaluateCondition(bp.condition, current_environment_)) {
                return true;
            }
        }
    }

    return false;
}
```

---

## Stepping

### Step Modes

**Step Over:**
- Execute next line
- Don't enter function calls
- Stop at next line in same function or caller

**Step Into:**
- Execute next line
- Enter function calls
- Stop at first line of called function

**Step Out:**
- Execute until current function returns
- Stop at line after function call in caller

**Continue:**
- Execute until next breakpoint or end

### Implementation

```cpp
class Debugger {
    void stepOver() {
        state_ = DebugState::SteppingOver;
        step_frame_depth_ = stack_manager_.getCurrentDepth();
        continue_();
    }

    void stepInto() {
        state_ = DebugState::SteppingInto;
        continue_();
    }

    void stepOut() {
        state_ = DebugState::SteppingOut;
        step_frame_depth_ = stack_manager_.getCurrentDepth() - 1;
        continue_();
    }

    void onBeforeStatement(const Stmt* stmt) {
        // Check breakpoints
        if (breakpoint_manager_.shouldBreak(stmt->getLocation())) {
            pause();
            return;
        }

        // Check step mode
        if (state_ == DebugState::SteppingOver) {
            if (stack_manager_.getCurrentDepth() <= step_frame_depth_) {
                pause();
                return;
            }
        } else if (state_ == DebugState::SteppingInto) {
            pause();
            return;
        } else if (state_ == DebugState::SteppingOut) {
            if (stack_manager_.getCurrentDepth() < step_frame_depth_) {
                pause();
                return;
            }
        }
    }

private:
    DebugState state_;
    int step_frame_depth_ = 0;
};
```

---

## Variable Inspection

### Scope Inspection

```cpp
std::vector<Variable> Debugger::getLocalVariables() {
    std::vector<Variable> variables;

    // Get current stack frame
    auto frame = stack_manager_.getCurrentFrame();

    // Get all variables in current scope
    for (const auto& [name, value] : frame.locals) {
        Variable var;
        var.name = name;
        var.type = value.getType();
        var.value = value;

        // For complex types, assign reference for expansion
        if (value.isStruct() || value.isList() || value.isDict()) {
            var.variablesReference = allocateVariableReference(value);
        } else {
            var.variablesReference = 0;
        }

        variables.push_back(var);
    }

    return variables;
}

Value Debugger::inspectVariable(const std::string& name) {
    // Search in current scope
    auto frame = stack_manager_.getCurrentFrame();
    if (frame.locals.count(name)) {
        return frame.locals[name];
    }

    // Search in parent scopes
    // ...

    throw DebugError("Variable '" + name + "' not found");
}
```

### Complex Type Expansion

**Struct:**
```cpp
std::vector<Variable> expandStruct(const Value& struct_value) {
    std::vector<Variable> fields;

    for (const auto& [field_name, field_value] : struct_value.getFields()) {
        Variable var;
        var.name = field_name;
        var.type = field_value.getType();
        var.value = field_value;

        if (field_value.isStruct() || field_value.isList() || field_value.isDict()) {
            var.variablesReference = allocateVariableReference(field_value);
        }

        fields.push_back(var);
    }

    return fields;
}
```

**List:**
```cpp
std::vector<Variable> expandList(const Value& list_value) {
    std::vector<Variable> elements;
    const auto& list = list_value.asList();

    for (size_t i = 0; i < list.size(); ++i) {
        Variable var;
        var.name = "[" + std::to_string(i) + "]";
        var.type = list[i].getType();
        var.value = list[i];

        if (list[i].isStruct() || list[i].isList() || list[i].isDict()) {
            var.variablesReference = allocateVariableReference(list[i]);
        }

        elements.push_back(var);
    }

    return elements;
}
```

---

## Stack Trace

### Stack Frame Tracking

```cpp
class StackManager {
public:
    void pushFrame(const std::string& function_name, const SourceLocation& location);
    void popFrame();

    std::vector<StackFrame> getStackTrace() const;
    StackFrame getCurrentFrame() const;
    int getCurrentDepth() const;

    void selectFrame(int frame_id);
    StackFrame getFrame(int frame_id) const;

private:
    std::vector<StackFrame> frames_;
    int selected_frame_id_ = 0;
};

void StackManager::pushFrame(const std::string& function_name, const SourceLocation& location) {
    StackFrame frame;
    frame.id = frames_.size();
    frame.name = function_name;
    frame.location = location;
    // locals will be populated by interpreter

    frames_.push_back(frame);
}

void StackManager::popFrame() {
    if (!frames_.empty()) {
        frames_.pop_back();
    }
}

std::vector<StackFrame> StackManager::getStackTrace() const {
    return frames_;
}
```

**Usage:**

```cpp
void Debugger::onFunctionEnter(const FunctionDecl* func, const std::vector<Value>& args) {
    stack_manager_.pushFrame(func->getName(), func->getLocation());

    // Populate locals with parameters
    auto& frame = stack_manager_.getCurrentFrame();
    const auto& params = func->getParams();
    for (size_t i = 0; i < params.size(); ++i) {
        frame.locals[params[i].name] = args[i];
    }
}

void Debugger::onFunctionExit(const FunctionDecl* func, const Value& return_value) {
    stack_manager_.popFrame();
}
```

---

## Watch Expressions

### Watch System

```cpp
struct WatchExpression {
    int id;
    std::string expression;
    bool enabled;
};

struct WatchResult {
    int watch_id;
    std::string expression;
    std::optional<Value> value;
    std::optional<std::string> error;
};

class Debugger {
    void addWatchExpression(const std::string& expression) {
        WatchExpression watch;
        watch.id = next_watch_id_++;
        watch.expression = expression;
        watch.enabled = true;
        watches_.push_back(watch);
    }

    std::vector<WatchResult> evaluateWatches() {
        std::vector<WatchResult> results;

        for (const auto& watch : watches_) {
            if (!watch.enabled) continue;

            WatchResult result;
            result.watch_id = watch.id;
            result.expression = watch.expression;

            try {
                // Parse expression
                Parser parser(watch.expression);
                auto expr = parser.parseExpression();

                // Evaluate in current scope
                Value value = interpreter_->evaluate(expr.get());
                result.value = value;
            } catch (const std::exception& e) {
                result.error = e.what();
            }

            results.push_back(result);
        }

        return results;
    }

private:
    std::vector<WatchExpression> watches_;
    int next_watch_id_ = 1;
};
```

---

## Debug Adapter Protocol (DAP)

### DAP Overview

**Protocol:** JSON-RPC over stdin/stdout
**Specification:** https://microsoft.github.io/debug-adapter-protocol/

**Message Types:**
- **Request:** Client → Adapter (e.g., setBreakpoints, continue, stackTrace)
- **Response:** Adapter → Client (success/error)
- **Event:** Adapter → Client (stopped, terminated, output)

### DAP Server

```cpp
class DAPServer {
public:
    DAPServer(Debugger* debugger);

    void run();  // Main loop

private:
    Debugger* debugger_;

    // Message handling
    void handleMessage(const json& message);
    void sendResponse(int request_id, const json& body);
    void sendEvent(const std::string& event, const json& body);

    // Request handlers
    void handleInitialize(const json& request);
    void handleSetBreakpoints(const json& request);
    void handleContinue(const json& request);
    void handleNext(const json& request);  // Step over
    void handleStepIn(const json& request);
    void handleStepOut(const json& request);
    void handleStackTrace(const json& request);
    void handleScopes(const json& request);
    void handleVariables(const json& request);
    void handleEvaluate(const json& request);
};
```

### Example: setBreakpoints Request

**Request:**
```json
{
  "type": "request",
  "seq": 1,
  "command": "setBreakpoints",
  "arguments": {
    "source": {
      "path": "/path/to/file.naab"
    },
    "breakpoints": [
      { "line": 10 },
      { "line": 20, "condition": "x > 5" }
    ]
  }
}
```

**Response:**
```json
{
  "type": "response",
  "request_seq": 1,
  "success": true,
  "command": "setBreakpoints",
  "body": {
    "breakpoints": [
      { "id": 1, "verified": true, "line": 10 },
      { "id": 2, "verified": true, "line": 20 }
    ]
  }
}
```

**Handler:**
```cpp
void DAPServer::handleSetBreakpoints(const json& request) {
    auto args = request["arguments"];
    std::string file = args["source"]["path"];

    // Clear existing breakpoints for this file
    debugger_->clearBreakpointsForFile(file);

    // Set new breakpoints
    json breakpoints_response = json::array();
    for (const auto& bp : args["breakpoints"]) {
        int line = bp["line"];

        int bp_id;
        if (bp.contains("condition")) {
            std::string condition = bp["condition"];
            bp_id = debugger_->setConditionalBreakpoint(file, line, condition);
        } else {
            bp_id = debugger_->setBreakpoint(file, line);
        }

        breakpoints_response.push_back({
            {"id", bp_id},
            {"verified", true},
            {"line", line}
        });
    }

    sendResponse(request["seq"], {{"breakpoints", breakpoints_response}});
}
```

### Example: Stopped Event

**Event (sent when breakpoint hit):**
```json
{
  "type": "event",
  "event": "stopped",
  "body": {
    "reason": "breakpoint",
    "threadId": 1,
    "allThreadsStopped": true
  }
}
```

---

## CLI Debugger

### Interactive Commands

```bash
# Start debugger
naab-debug program.naab

# Commands
(naab-debug) break file.naab:10      # Set breakpoint
(naab-debug) break myFunction        # Function breakpoint
(naab-debug) run                     # Start execution
(naab-debug) next                    # Step over
(naab-debug) step                    # Step into
(naab-debug) finish                  # Step out
(naab-debug) continue                # Continue execution
(naab-debug) print x                 # Inspect variable
(naab-debug) backtrace               # Show stack trace
(naab-debug) frame 2                 # Select stack frame
(naab-debug) watch x > 5             # Watch expression
(naab-debug) info breakpoints        # List breakpoints
(naab-debug) delete 1                # Remove breakpoint
(naab-debug) quit                    # Exit debugger
```

### CLI Implementation

```cpp
class CLIDebugger {
public:
    CLIDebugger(Debugger* debugger);

    void run();

private:
    Debugger* debugger_;
    std::map<std::string, std::function<void(const std::vector<std::string>&)>> commands_;

    void registerCommands();
    void handleCommand(const std::string& line);

    // Command handlers
    void cmdBreak(const std::vector<std::string>& args);
    void cmdRun(const std::vector<std::string>& args);
    void cmdNext(const std::vector<std::string>& args);
    void cmdStep(const std::vector<std::string>& args);
    void cmdContinue(const std::vector<std::string>& args);
    void cmdPrint(const std::vector<std::string>& args);
    void cmdBacktrace(const std::vector<std::string>& args);
};
```

---

## Interpreter Instrumentation

### Instrumentation Points

**Before/After Every Statement:**
```cpp
void Interpreter::visitStmt(Stmt* stmt) {
    if (debugger_) {
        debugger_->onBeforeStatement(stmt);
    }

    // Execute statement
    stmt->accept(this);

    if (debugger_) {
        debugger_->onAfterStatement(stmt);
    }
}
```

**Function Enter/Exit:**
```cpp
Value Interpreter::visitFunctionCall(FunctionCall* call) {
    auto func = evaluate(call->getCallee());
    auto args = evaluateArguments(call->getArguments());

    if (debugger_) {
        debugger_->onFunctionEnter(func, args);
    }

    Value result = executeFunction(func, args);

    if (debugger_) {
        debugger_->onFunctionExit(func, result);
    }

    return result;
}
```

**Performance Overhead:**
- No overhead when debugger not attached
- Minimal overhead when debugger attached but not paused (~5-10%)

---

## VS Code Integration

### Launch Configuration

**.vscode/launch.json:**
```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "naab",
      "request": "launch",
      "name": "Debug NAAb Program",
      "program": "${workspaceFolder}/main.naab",
      "stopOnEntry": false,
      "cwd": "${workspaceFolder}"
    }
  ]
}
```

### Extension

**package.json:**
```json
{
  "name": "naab-debug",
  "displayName": "NAAb Debug",
  "version": "0.1.0",
  "contributes": {
    "debuggers": [{
      "type": "naab",
      "label": "NAAb Debug",
      "program": "./out/debugAdapter.js",
      "configurationAttributes": {
        "launch": {
          "required": ["program"],
          "properties": {
            "program": {
              "type": "string",
              "description": "Path to NAAb program"
            }
          }
        }
      }
    }]
  }
}
```

**Debug Adapter:**
```typescript
import * as vscode from 'vscode';
import { spawn } from 'child_process';

export function activate(context: vscode.ExtensionContext) {
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('naab', {
            createDebugAdapterDescriptor(session: vscode.DebugSession) {
                // Launch naab-debug as DAP server
                const debugServer = spawn('naab-debug', ['--dap'], {
                    stdio: ['pipe', 'pipe', 'inherit']
                });

                return new vscode.DebugAdapterServer(
                    debugServer.stdout,
                    debugServer.stdin
                );
            }
        })
    );
}
```

---

## Implementation Plan

### Week 1: Core Debugger (5 days)

- [ ] Implement Debugger class
- [ ] Implement BreakpointManager
- [ ] Implement StackManager
- [ ] Instrument interpreter
- [ ] Test: Breakpoints and stepping work

### Week 2: Variable Inspection (5 days)

- [ ] Implement variable inspection
- [ ] Complex type expansion (struct, list, dict)
- [ ] Watch expressions
- [ ] Test: Can inspect all types

### Week 3: CLI Debugger (5 days)

- [ ] Implement CLIDebugger
- [ ] Interactive commands
- [ ] Output formatting
- [ ] Test: CLI debugger usable

### Week 4: DAP Server (5 days)

- [ ] Implement DAPServer
- [ ] Handle all DAP requests
- [ ] Send DAP events
- [ ] Test: DAP protocol compliance

### Week 5: VS Code Integration (5 days)

- [ ] Create VS Code extension
- [ ] Debug adapter implementation
- [ ] UI integration (breakpoints, variables, stack)
- [ ] Test: Debug in VS Code

**Total: 5 weeks**

---

## Testing Strategy

### Unit Tests

```cpp
TEST(DebuggerTest, SetBreakpoint) {
    Debugger debugger(&interpreter);
    int bp_id = debugger.setBreakpoint("test.naab", 10);
    ASSERT_GT(bp_id, 0);
}

TEST(DebuggerTest, StepOver) {
    Debugger debugger(&interpreter);
    debugger.setBreakpoint("test.naab", 5);
    debugger.run();  // Pauses at breakpoint
    debugger.stepOver();
    // Verify execution is at line 6
}
```

### Integration Tests

```naab
// test_debug.naab
function factorial(n: int) -> int {
    if (n <= 1) {
        return 1  // Breakpoint here
    }
    return n * factorial(n - 1)
}

let result = factorial(5)
```

**Test:**
1. Set breakpoint at line 3
2. Run program
3. Verify execution pauses at line 3
4. Inspect variable `n` (should be 5)
5. Step into recursive call
6. Verify n is now 4

---

## Success Metrics

### Phase 4.4 Complete When:

- [x] Debugger core implemented
- [x] Breakpoints working (line, function, conditional)
- [x] Stepping working (over, into, out)
- [x] Variable inspection working
- [x] Stack trace navigation working
- [x] CLI debugger usable
- [x] DAP server compliant
- [x] VS Code extension published
- [x] Documentation complete

---

## Conclusion

**Phase 4.4 Status: DESIGN COMPLETE**

An interactive debugger will:
- Dramatically improve debugging experience
- Enable efficient troubleshooting
- Match professional development tools

**Implementation Effort:** 5 weeks

**Priority:** High (essential for complex application development)

**Dependencies:** Instrumented interpreter, source location tracking

Once implemented, NAAb will have debugging capabilities on par with Python, JavaScript, or Go.
