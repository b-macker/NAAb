# Phase 4.3: Linter (naab-lint) - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** Medium-High - Static analysis and rule engine
**Estimated Effort:** 2-3 weeks implementation
**Priority:** MEDIUM - Important for code quality

This document outlines the design for `naab-lint`, a static analysis tool for NAAb that detects bugs, code smells, and style issues before runtime.

---

## Current Problem

**No Static Analysis:**
- Bugs discovered only at runtime
- No detection of unused variables/functions
- No warnings for common mistakes
- No enforcement of best practices
- Code quality varies across projects

**Impact:** More bugs reach production, wasted debugging time, inconsistent code quality.

---

## Goals

### Primary Goals

1. **Bug Prevention** - Catch common errors before runtime
2. **Code Quality** - Enforce best practices
3. **Performance** - Fast enough for real-time IDE integration
4. **Configurable** - Rules can be enabled/disabled per project
5. **Actionable** - Error messages suggest fixes

### Secondary Goals

6. **Extensible** - Easy to add new rules
7. **IDE Integration** - Real-time linting via LSP
8. **CI Integration** - Fail builds on lint errors

---

## Lint Rules

### Category 1: Correctness (Bugs)

#### Rule: unused-variable

**Description:** Variable declared but never used

**Example:**
```naab
function processData(input: string) {
    let result = parseInput(input)  // Used
    let temp = 42  // UNUSED - Warning!
    return result
}
```

**Message:** `Variable 'temp' is declared but never used`

**Suggestion:** `Remove unused variable or prefix with '_' if intentional`

**Severity:** Warning

---

#### Rule: unused-function

**Description:** Function defined but never called

**Example:**
```naab
function helperFunction() {  // UNUSED - Warning!
    return 42
}

function main() {
    print("Hello")
}
```

**Message:** `Function 'helperFunction' is defined but never called`

**Suggestion:** `Remove unused function or export it if it's part of public API`

**Severity:** Warning

---

#### Rule: dead-code

**Description:** Code after return/break/continue that can never execute

**Example:**
```naab
function getValue() -> int {
    return 42
    print("This never runs")  // DEAD CODE - Warning!
}
```

**Message:** `Unreachable code detected after 'return' statement`

**Suggestion:** `Remove unreachable code`

**Severity:** Warning

---

#### Rule: nullable-access

**Description:** Accessing nullable value without null check

**Example:**
```naab
let value: string? = getValue()
print(value.length)  // UNSAFE - Error!
// Should check: if (value != null) { ... }
```

**Message:** `Accessing member 'length' on nullable type 'string?' without null check`

**Suggestion:** `Add null check: if (value != null) { ... }`

**Severity:** Error

---

#### Rule: division-by-zero

**Description:** Division by zero literal

**Example:**
```naab
let result = x / 0  // Error!
```

**Message:** `Division by zero`

**Severity:** Error

---

#### Rule: infinite-loop

**Description:** Loop without break condition

**Example:**
```naab
loop {
    print("Forever")
    // No break statement - Warning!
}
```

**Message:** `Infinite loop detected (no 'break' statement)`

**Suggestion:** `Add 'break' condition or ensure loop will terminate`

**Severity:** Warning

---

### Category 2: Type Safety

#### Rule: implicit-any

**Description:** Variable without type annotation and cannot infer

**Example:**
```naab
let x  // No initializer, no type - Error!
```

**Message:** `Cannot infer type for variable 'x' (no initializer)`

**Suggestion:** `Add type annotation: let x: int`

**Severity:** Error

---

#### Rule: type-mismatch

**Description:** Assigning incompatible types

**Example:**
```naab
let x: int = "hello"  // Error!
```

**Message:** `Type mismatch: cannot assign 'string' to 'int'`

**Severity:** Error

---

#### Rule: missing-return

**Description:** Function has non-void return type but missing return

**Example:**
```naab
function getValue() -> int {
    if (condition) {
        return 42
    }
    // Missing return for else branch - Error!
}
```

**Message:** `Function 'getValue' with return type 'int' may not return a value in all code paths`

**Suggestion:** `Add return statement in all branches`

**Severity:** Error

---

### Category 3: Best Practices

#### Rule: large-function

**Description:** Function has too many lines (> 100)

**Example:**
```naab
function processEverything() {
    // 150 lines of code
    // ...
}  // Warning: Function too large!
```

**Message:** `Function 'processEverything' has 150 lines (max: 100)`

**Suggestion:** `Break into smaller functions`

**Severity:** Warning

---

#### Rule: complex-function

**Description:** Function has high cyclomatic complexity (> 10)

**Example:**
```naab
function complexLogic() {
    if (...) {
        if (...) {
            if (...) {
                if (...) {
                    // Too many nested conditions - Warning!
                }
            }
        }
    }
}
```

**Message:** `Function 'complexLogic' has cyclomatic complexity of 15 (max: 10)`

**Suggestion:** `Simplify logic or extract methods`

**Severity:** Warning

---

#### Rule: magic-number

**Description:** Numeric literal used directly (should be const)

**Example:**
```naab
function calculateTax(amount: float) -> float {
    return amount * 0.08  // Magic number! What is 0.08?
}

// Better:
const TAX_RATE = 0.08
function calculateTax(amount: float) -> float {
    return amount * TAX_RATE
}
```

**Message:** `Magic number '0.08' used directly`

**Suggestion:** `Extract to named constant`

**Severity:** Info (lowest severity)

---

#### Rule: long-parameter-list

**Description:** Function has too many parameters (> 5)

**Example:**
```naab
function process(a: int, b: int, c: int, d: int, e: int, f: int) {
    // 6 parameters - too many!
}
```

**Message:** `Function 'process' has 6 parameters (max: 5)`

**Suggestion:** `Use struct to group related parameters`

**Severity:** Warning

---

### Category 4: Style

#### Rule: naming-convention

**Description:** Identifier doesn't follow naming conventions

**Conventions:**
- Functions: camelCase (`processData`)
- Variables: camelCase (`myVariable`)
- Structs: PascalCase (`Person`)
- Enums: PascalCase (`Status`)
- Constants: UPPER_SNAKE_CASE (`MAX_SIZE`)

**Example:**
```naab
function ProcessData() {  // Warning: should be camelCase
    // ...
}

struct person {  // Warning: should be PascalCase
    // ...
}

const max_size = 100  // Warning: should be UPPER_SNAKE_CASE
```

**Message:** `Function 'ProcessData' should use camelCase naming`

**Severity:** Info

---

#### Rule: inconsistent-return

**Description:** Function has multiple return statements (code smell)

**Example:**
```naab
function getValue(x: int) -> int {
    if (x > 0) {
        return x
    }
    if (x < 0) {
        return -x
    }
    return 0
}
// 3 return statements - consider refactoring
```

**Message:** `Function has 3 return statements (consider refactoring)`

**Severity:** Info

---

### Category 5: Performance

#### Rule: unnecessary-copy

**Description:** Passing large struct by value instead of reference

**Example:**
```naab
function processData(data: LargeDataset) {  // Warning: passing by value
    // ... LargeDataset is copied
}

// Better:
function processData(ref data: LargeDataset) {  // Pass by reference
    // ... No copy
}
```

**Message:** `Large struct 'LargeDataset' passed by value (use 'ref' for efficiency)`

**Severity:** Info

---

#### Rule: string-concatenation-loop

**Description:** String concatenation in loop (O(n²) complexity)

**Example:**
```naab
let result = ""
for i in 0..1000 {
    result = result + i.toString()  // Inefficient!
}

// Better: Use string builder or list + join
```

**Message:** `String concatenation in loop (O(n²) complexity)`

**Suggestion:** `Use list + join or string builder`

**Severity:** Warning

---

## Architecture

### Project Structure

```
tools/naab-lint/
├── main.cpp                  # CLI entry point
├── linter.h/cpp             # Main linter
├── rule_engine.h/cpp        # Rule execution engine
├── config.h/cpp             # Configuration (.naablintrc)
├── rules/                   # Lint rules
│   ├── rule.h               # Base Rule interface
│   ├── unused_variable.cpp
│   ├── unused_function.cpp
│   ├── dead_code.cpp
│   ├── nullable_access.cpp
│   ├── large_function.cpp
│   └── ...
└── visitors/                # AST visitors for analysis
    ├── symbol_collector.cpp
    ├── usage_tracker.cpp
    └── complexity_analyzer.cpp
```

### Core Components

#### 1. Linter (linter.cpp)

**Responsibilities:**
- Coordinate linting process
- Parse → Analyze → Report

```cpp
class Linter {
public:
    Linter(const LintConfig& config);

    // Lint single file
    std::vector<Diagnostic> lintFile(const std::string& path);

    // Lint multiple files
    std::vector<Diagnostic> lintFiles(const std::vector<std::string>& paths);

private:
    LintConfig config_;
    RuleEngine rule_engine_;
};
```

#### 2. Rule Engine (rule_engine.cpp)

**Responsibilities:**
- Manage lint rules
- Execute rules on AST
- Collect diagnostics

```cpp
class RuleEngine {
public:
    // Register rule
    void registerRule(std::unique_ptr<Rule> rule);

    // Run all enabled rules on AST
    std::vector<Diagnostic> runRules(const ast::Program* program);

private:
    std::vector<std::unique_ptr<Rule>> rules_;
};
```

#### 3. Rule Interface (rules/rule.h)

**Responsibilities:**
- Base interface for all lint rules

```cpp
class Rule {
public:
    virtual ~Rule() = default;

    // Rule metadata
    virtual std::string getId() const = 0;  // e.g., "unused-variable"
    virtual std::string getName() const = 0;
    virtual std::string getDescription() const = 0;
    virtual Severity getDefaultSeverity() const = 0;

    // Run rule on AST
    virtual std::vector<Diagnostic> check(const ast::Program* program) = 0;
};

enum class Severity {
    Error,    // Prevents compilation (if strict mode)
    Warning,  // Should fix
    Info      // Suggestion
};
```

#### 4. Example Rule Implementation (unused_variable.cpp)

```cpp
class UnusedVariableRule : public Rule {
public:
    std::string getId() const override { return "unused-variable"; }
    std::string getName() const override { return "Unused Variable"; }
    std::string getDescription() const override {
        return "Detects variables that are declared but never used";
    }
    Severity getDefaultSeverity() const override { return Severity::Warning; }

    std::vector<Diagnostic> check(const ast::Program* program) override {
        std::vector<Diagnostic> diagnostics;

        // Step 1: Collect all variable declarations
        SymbolCollector collector;
        auto symbols = collector.collect(program);

        // Step 2: Track variable usages
        UsageTracker tracker;
        auto usages = tracker.track(program);

        // Step 3: Find unused variables
        for (const auto& symbol : symbols) {
            if (symbol.kind == SymbolKind::Variable) {
                if (usages.count(symbol.name) == 0) {
                    // Variable declared but never used
                    Diagnostic diag;
                    diag.range = symbol.location.toRange();
                    diag.severity = Severity::Warning;
                    diag.code = "unused-variable";
                    diag.message = "Variable '" + symbol.name + "' is declared but never used";
                    diag.suggestion = "Remove unused variable or prefix with '_' if intentional";
                    diagnostics.push_back(diag);
                }
            }
        }

        return diagnostics;
    }
};
```

#### 5. Configuration (config.cpp)

**Responsibilities:**
- Load lint configuration
- Enable/disable rules
- Set severity levels

```cpp
struct RuleConfig {
    bool enabled = true;
    std::optional<Severity> severity;  // Override default
};

struct LintConfig {
    std::map<std::string, RuleConfig> rules;

    // Load from .naablintrc
    static LintConfig load(const std::string& path);

    // Default config (all rules enabled)
    static LintConfig defaultConfig();
};
```

**Example .naablintrc:**

```json
{
  "rules": {
    "unused-variable": {
      "enabled": true,
      "severity": "warning"
    },
    "magic-number": {
      "enabled": false
    },
    "large-function": {
      "enabled": true,
      "severity": "info",
      "maxLines": 150
    }
  }
}
```

---

## Analysis Algorithms

### Unused Variable Detection

**Algorithm:**

1. **Collect Declarations:** Walk AST, find all `VarDeclStmt` nodes
2. **Track Usages:** Walk AST, find all `IdentifierExpr` nodes
3. **Compare:** Variables in declarations but not in usages = unused

```cpp
class UnusedVariableDetector {
public:
    std::vector<std::string> findUnused(const ast::Program* program) {
        // Step 1: Collect declarations
        std::set<std::string> declared;
        for (auto& decl : program->declarations) {
            if (auto* var_decl = dynamic_cast<VarDeclStmt*>(decl.get())) {
                declared.insert(var_decl->getName());
            }
        }

        // Step 2: Track usages
        std::set<std::string> used;
        UsageTracker tracker;
        tracker.visit(program, [&](const IdentifierExpr* expr) {
            used.insert(expr->getName());
        });

        // Step 3: Find unused
        std::vector<std::string> unused;
        for (const auto& var : declared) {
            if (used.count(var) == 0) {
                unused.push_back(var);
            }
        }

        return unused;
    }
};
```

### Dead Code Detection

**Algorithm:**

1. **Walk Function Bodies:** Find `return`, `break`, `continue` statements
2. **Check Following Statements:** Any statement after these is dead code

```cpp
class DeadCodeDetector {
public:
    std::vector<const Stmt*> findDeadCode(const ast::FunctionDecl* func) {
        std::vector<const Stmt*> dead;

        // Walk compound statements
        if (auto* body = dynamic_cast<CompoundStmt*>(func->getBody())) {
            bool unreachable = false;
            for (const auto& stmt : body->getStatements()) {
                if (unreachable) {
                    dead.push_back(stmt.get());
                }

                // After return/break/continue, code is unreachable
                if (isTerminator(stmt.get())) {
                    unreachable = true;
                }
            }
        }

        return dead;
    }

private:
    bool isTerminator(const Stmt* stmt) {
        return dynamic_cast<const ReturnStmt*>(stmt) ||
               dynamic_cast<const BreakStmt*>(stmt) ||
               dynamic_cast<const ContinueStmt*>(stmt);
    }
};
```

### Cyclomatic Complexity

**Algorithm:**

McCabe's Cyclomatic Complexity = E - N + 2P

Where:
- E = number of edges in control flow graph
- N = number of nodes
- P = number of connected components

**Simplified:**
Complexity = 1 + number of decision points (if, for, while, &&, ||, case)

```cpp
class ComplexityAnalyzer {
public:
    int calculateComplexity(const ast::FunctionDecl* func) {
        int complexity = 1;  // Base complexity

        // Count decision points
        ComplexityVisitor visitor;
        visitor.visit(func->getBody(), [&](const Stmt* stmt) {
            if (dynamic_cast<const IfStmt*>(stmt) ||
                dynamic_cast<const ForStmt*>(stmt) ||
                dynamic_cast<const WhileStmt*>(stmt)) {
                complexity++;
            }
        });

        // Count logical operators
        visitor.visit(func->getBody(), [&](const Expr* expr) {
            if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
                if (binary->getOp() == TokenType::AND ||
                    binary->getOp() == TokenType::OR) {
                    complexity++;
                }
            }
        });

        return complexity;
    }
};
```

---

## CLI Interface

### Commands

```bash
# Lint single file
naab-lint file.naab

# Lint multiple files
naab-lint file1.naab file2.naab

# Lint directory
naab-lint src/

# Output formats
naab-lint --format=json file.naab  # JSON output
naab-lint --format=text file.naab  # Human-readable (default)

# Severity filtering
naab-lint --errors-only file.naab  # Only errors
naab-lint --warnings file.naab     # Errors + warnings

# Configuration
naab-lint --config .naablintrc file.naab

# Exit codes
naab-lint file.naab  # 0 if no issues, 1 if warnings, 2 if errors
```

### Output Format

**Text (Human-Readable):**

```
file.naab:12:5: warning: Variable 'temp' is declared but never used (unused-variable)
   10 | function processData(input: string) {
   11 |     let result = parseInput(input)
   12 |     let temp = 42
       |         ^^^^ unused variable
   13 |     return result
   14 | }
Suggestion: Remove unused variable or prefix with '_' if intentional

file.naab:25:12: error: Accessing member 'length' on nullable type 'string?' (nullable-access)
   23 | let value: string? = getValue()
   24 | print(value.length)
       |       ^^^^^
Suggestion: Add null check: if (value != null) { ... }

✖ 2 problems (1 error, 1 warning)
```

**JSON (Machine-Readable):**

```json
{
  "diagnostics": [
    {
      "file": "file.naab",
      "range": {
        "start": { "line": 12, "character": 5 },
        "end": { "line": 12, "character": 9 }
      },
      "severity": "warning",
      "code": "unused-variable",
      "message": "Variable 'temp' is declared but never used",
      "suggestion": "Remove unused variable or prefix with '_' if intentional"
    }
  ],
  "summary": {
    "errorCount": 1,
    "warningCount": 1,
    "infoCount": 0
  }
}
```

---

## LSP Integration

### Real-Time Linting

**Integration with naab-lsp:**

```cpp
class DiagnosticsProvider {
public:
    std::vector<Diagnostic> getDiagnostics(const Document& doc) {
        std::vector<Diagnostic> diagnostics;

        // 1. Parse errors
        diagnostics = collectParseErrors(doc.ast.get());

        // 2. Type errors
        auto type_errors = runTypeChecker(doc.ast.get());
        diagnostics.insert(diagnostics.end(), type_errors.begin(), type_errors.end());

        // 3. Lint warnings (NEW)
        Linter linter(LintConfig::defaultConfig());
        auto lint_diagnostics = linter.lint(doc.ast.get());
        diagnostics.insert(diagnostics.end(), lint_diagnostics.begin(), lint_diagnostics.end());

        return diagnostics;
    }
};
```

**Effect:** Lint warnings show in editor in real-time as you type.

---

## Testing Strategy

### Unit Tests

**Test Each Rule:**

```cpp
TEST(LinterTest, UnusedVariable) {
    std::string code = R"(
        function foo() {
            let x = 42  // Used
            let y = 10  // Unused
            return x
        }
    )";

    Linter linter(LintConfig::defaultConfig());
    auto diagnostics = linter.lintString(code);

    ASSERT_EQ(diagnostics.size(), 1);
    ASSERT_EQ(diagnostics[0].code, "unused-variable");
    ASSERT_EQ(diagnostics[0].message, "Variable 'y' is declared but never used");
}
```

### Integration Tests

**Golden Files:**

```
tests/linter/
├── input/
│   ├── unused_variable.naab
│   ├── dead_code.naab
│   └── complex_function.naab
└── expected/
    ├── unused_variable.json
    ├── dead_code.json
    └── complex_function.json
```

---

## Implementation Plan

### Week 1: Core Infrastructure (5 days)

- [ ] Set up naab-lint project structure
- [ ] Implement Linter class
- [ ] Implement Rule Engine
- [ ] Implement Rule interface
- [ ] Test: Can register and run rules

### Week 2: Core Rules (5 days)

- [ ] Implement unused-variable rule
- [ ] Implement unused-function rule
- [ ] Implement dead-code rule
- [ ] Implement nullable-access rule
- [ ] Test: Core rules work correctly

### Week 3: Additional Rules & Polish (5 days)

- [ ] Implement best practice rules (large-function, complex-function, etc.)
- [ ] Implement style rules (naming conventions)
- [ ] Add configuration support (.naablintrc)
- [ ] CLI interface
- [ ] Documentation

**Total: 3 weeks**

---

## Success Metrics

### Phase 4.3 Complete When:

- [x] naab-lint implements 15+ lint rules
- [x] Rules categorized by severity (Error, Warning, Info)
- [x] Configuration system working (.naablintrc)
- [x] CLI interface complete
- [x] LSP integration (real-time linting in IDE)
- [x] JSON output for CI integration
- [x] Documentation complete
- [x] Test coverage >90%

---

## Conclusion

**Phase 4.3 Status: DESIGN COMPLETE**

A comprehensive linter will:
- Catch bugs before runtime
- Enforce code quality standards
- Improve code maintainability
- Reduce technical debt

**Implementation Effort:** 3 weeks

**Priority:** Medium (important for production code quality)

**Dependencies:** Parser, Type Checker, LSP (for IDE integration)

Once implemented, NAAb will have static analysis on par with ESLint, Clippy (Rust), or golangci-lint.
