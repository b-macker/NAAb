# Phase 4.2: Auto-Formatter (naab-fmt) - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** Medium - AST pretty-printing with style rules
**Estimated Effort:** 1-2 weeks implementation
**Priority:** MEDIUM - Important for code consistency

This document outlines the design for `naab-fmt`, an automatic code formatter for NAAb that enforces consistent code style across projects.

---

## Current Problem

**No Code Formatter:**
- Inconsistent code style across files/projects
- Manual formatting is tedious and error-prone
- No agreed-upon style guide
- Code reviews waste time on style discussions
- Hard to maintain consistency in teams

**Impact:** Poor code readability, wasted developer time, inconsistent codebase.

---

## Goals

### Primary Goals

1. **Consistent Style** - All NAAb code looks the same
2. **Zero Configuration** - Works out of the box with sensible defaults
3. **Preserves Semantics** - Never changes program behavior
4. **Idempotent** - Formatting twice produces same result
5. **Fast** - Format entire file in <100ms

### Secondary Goals

6. **Configurable** - Allow some style preferences
7. **Editor Integration** - Format-on-save in VS Code
8. **CI Integration** - Check formatting in CI/CD

---

## Design Philosophy

### Inspiration

**Model After:**
- **Prettier (JavaScript)** - Opinionated, zero-config
- **gofmt (Go)** - Standard formatter, no arguments
- **rustfmt (Rust)** - Configurable but opinionated
- **Black (Python)** - "The uncompromising formatter"

**Key Principle:** "There should be one-- and preferably only one --obvious way to format NAAb code."

### Opinionated vs Configurable

**Default Stance:** Opinionated (like gofmt, Black)
- Most style rules are NOT configurable
- Reduces bikeshedding
- Ensures consistency across ecosystem

**Exceptions:** A few configurable options
- Indentation width (2 vs 4 spaces, default: 4)
- Max line length (default: 100)
- Semicolons (always vs never vs as-needed, default: never)

---

## Style Guide

### 1. Indentation

**Rule:** 4 spaces (configurable: 2 or 4)

```naab
// Good
function add(a: int, b: int) -> int {
    return a + b
}

// Bad (2 spaces)
function add(a: int, b: int) -> int {
  return a + b
}
```

**Rationale:** 4 spaces is more readable than 2, standard for many languages.

---

### 2. Brace Style

**Rule:** Egyptian/K&R style (opening brace on same line)

```naab
// Good
function foo() {
    // ...
}

struct Person {
    name: string
}

if condition {
    // ...
} else {
    // ...
}

// Bad (Allman style)
function foo()
{
    // ...
}
```

**Rationale:** More compact, standard in C/Java/JavaScript.

---

### 3. Semicolons

**Rule:** Never (default), unless configured otherwise

```naab
// Good (no semicolons)
let x = 42
let y = "hello"
print(x)

// Bad (unnecessary semicolons)
let x = 42;
let y = "hello";
print(x);
```

**Exception:** Semicolons allowed/required for multi-statement lines (if feature added)

**Rationale:** Modern languages (JavaScript, Swift, Kotlin) make semicolons optional.

---

### 4. Line Length

**Rule:** Max 100 characters (configurable: 80-120)

```naab
// Good (under 100 chars)
function processData(input: string, options: Options) -> Result<Data, Error> {
    // ...
}

// Bad (over 100 chars)
function processDataWithVeryLongNameAndManyParameters(input: string, options: Options, config: Config, logger: Logger) -> Result<Data, Error> {
    // ...
}

// Good (wrapped)
function processDataWithVeryLongNameAndManyParameters(
    input: string,
    options: Options,
    config: Config,
    logger: Logger
) -> Result<Data, Error> {
    // ...
}
```

**Rationale:** 100 characters is comfortable on modern screens, allows side-by-side diffs.

---

### 5. Function Declarations

**Rule:** Parameters wrapped if total length > max line length

```naab
// Short: single line
function add(a: int, b: int) -> int {
    return a + b
}

// Long: wrap parameters, one per line
function processTransaction(
    accountId: string,
    amount: float,
    currency: string,
    timestamp: int
) -> Result<Transaction, Error> {
    // ...
}

// Align closing paren with opening, or on new line
function processTransaction(
    accountId: string,
    amount: float
) -> Result<Transaction, Error> {  // Closing paren same line as return type
    // ...
}
```

---

### 6. Function Calls

**Rule:** Wrap arguments if total length > max line length

```naab
// Short: single line
let result = add(1, 2)

// Long: wrap arguments, one per line
let transaction = processTransaction(
    accountId: "12345",
    amount: 100.50,
    currency: "USD",
    timestamp: getCurrentTime()
)

// Trailing comma allowed
let transaction = processTransaction(
    accountId: "12345",
    amount: 100.50,
    currency: "USD",
    timestamp: getCurrentTime(),  // Trailing comma
)
```

---

### 7. Struct Declarations

**Rule:** Fields indented, one per line

```naab
// Good
struct Person {
    name: string
    age: int
    email: string
}

// Bad (single line for small structs - not allowed)
struct Person { name: string, age: int }

// Blank line between struct fields and methods (if methods added)
struct Person {
    name: string
    age: int

    function greet() {
        print("Hello, " + self.name)
    }
}
```

---

### 8. Struct Literals

**Rule:** Multi-line if > 2 fields OR total length > max line length

```naab
// Short: single line (2 or fewer fields)
let point = Point { x: 10, y: 20 }

// Long: multi-line (3+ fields or long)
let person = Person {
    name: "Alice",
    age: 30,
    email: "alice@example.com"
}

// Trailing comma allowed
let person = Person {
    name: "Alice",
    age: 30,
    email: "alice@example.com",  // Trailing comma
}

// Nested structs: indent properly
let company = Company {
    name: "TechCorp",
    ceo: Person {
        name: "Bob",
        age: 45
    },
    founded: 2000
}
```

---

### 9. Lists and Dicts

**Rule:** Single line if short, multi-line if long

```naab
// Short lists: single line
let numbers = [1, 2, 3, 4, 5]

// Long lists: multi-line, one element per line
let numbers = [
    1,
    2,
    3,
    4,
    5
]

// Dicts: same rule
let config = { "host": "localhost", "port": 8080 }

let config = {
    "host": "localhost",
    "port": 8080,
    "timeout": 30
}
```

---

### 10. If/Else Statements

**Rule:** Braces always required, even for single statement

```naab
// Good
if condition {
    doSomething()
}

// Bad (no braces for single statement - not allowed)
if condition
    doSomething()

// Else on same line as closing brace
if condition {
    doSomething()
} else {
    doSomethingElse()
}

// Else if: same line
if condition1 {
    // ...
} else if condition2 {
    // ...
} else {
    // ...
}
```

**Rationale:** Always using braces prevents errors when adding statements.

---

### 11. Loops

**Rule:** Braces always required

```naab
// For loop
for i in 0..10 {
    print(i)
}

// While loop
while condition {
    doSomething()
}

// Loop (infinite)
loop {
    // ...
    if shouldBreak {
        break
    }
}
```

---

### 12. Binary Operations

**Rule:** Spaces around operators

```naab
// Good
let sum = a + b
let result = x * 2 + y / 3
let isValid = age >= 18 && hasPermission

// Bad (no spaces)
let sum = a+b
let result = x*2+y/3
```

**Exception:** No space for unary operators

```naab
let negative = -x
let notTrue = !isValid
```

---

### 13. Comments

**Rule:**
- Single space after `//`
- Block comments use `/* */` with proper indentation
- Doc comments use `///`

```naab
// Good: single-line comment

/*
 * Good: multi-line comment
 * with proper formatting
 */

/// Good: documentation comment
/// for functions/structs

// Bad: no space after //
/*Bad: no proper formatting*/
```

---

### 14. Blank Lines

**Rule:**
- One blank line between top-level declarations
- No blank lines at start/end of blocks
- One blank line before comments (optional)

```naab
// Good
function foo() {
    let x = 42
    return x
}

function bar() {
    let y = 24
    return y
}

// Bad (no blank line between functions)
function foo() {
    let x = 42
    return x
}
function bar() {
    let y = 24
    return y
}

// Bad (blank lines at start/end of block)
function foo() {

    let x = 42
    return x

}
```

---

### 15. Inline Code

**Rule:** Preserve content, format only NAAb wrapper

```naab
// Good: inline code left as-is
let result = <<python
import numpy as np
data = np.array([1, 2, 3])
return data.sum()
>>

// Format the assignment, not the inline code
let result = <<python import numpy as np; return np.sum([1,2,3])>>

// If very long, can wrap the inline code block
let result = <<python
    import numpy as np
    data = np.array([1, 2, 3, 4, 5])
    return data.sum()
>>
```

**Rationale:** Inline code is in different language, shouldn't be formatted by naab-fmt.

---

### 16. Imports (Future Feature)

**Rule:** Group and sort imports

```naab
// Good
import "std/io"
import "std/json"
import "std/http"

import "my/lib/utils"
import "my/lib/data"

// Bad (not sorted, not grouped)
import "my/lib/utils"
import "std/json"
import "my/lib/data"
import "std/io"
```

**Groups:**
1. Standard library imports
2. Third-party imports
3. Local imports

**Within each group:** Alphabetically sorted

---

## Implementation Architecture

### Project Structure

```
tools/naab-fmt/
├── main.cpp              # CLI entry point
├── formatter.h/cpp       # Main formatter
├── style_config.h/cpp    # Style configuration
├── printer.h/cpp         # AST pretty-printer
└── rules/
    ├── indentation.h/cpp
    ├── braces.h/cpp
    ├── line_length.h/cpp
    └── whitespace.h/cpp
```

### Core Components

#### 1. Formatter (formatter.cpp)

**Responsibilities:**
- Coordinate formatting process
- Parse → Format → Print

```cpp
class Formatter {
public:
    Formatter(const StyleConfig& config);

    // Format file in-place
    bool formatFile(const std::string& path);

    // Format string, return result
    std::string formatString(const std::string& code);

    // Check if file is formatted (for CI)
    bool isFormatted(const std::string& path);

private:
    StyleConfig config_;
    Printer printer_;
};
```

#### 2. Style Config (style_config.cpp)

**Responsibilities:**
- Load style configuration
- Provide defaults

```cpp
struct StyleConfig {
    int indentWidth = 4;          // 2 or 4
    int maxLineLength = 100;      // 80-120
    bool trailingCommas = true;   // Allow trailing commas
    SemicolonStyle semicolons = SemicolonStyle::Never;  // Never, Always, AsNeeded

    // Load from .naabfmt config file
    static StyleConfig load(const std::string& path);

    // Default config
    static StyleConfig defaultConfig();
};
```

#### 3. Printer (printer.cpp)

**Responsibilities:**
- Pretty-print AST to formatted code
- Apply style rules

```cpp
class Printer {
public:
    Printer(const StyleConfig& config);

    // Print entire program
    std::string print(const ast::Program* program);

private:
    StyleConfig config_;
    int indent_level_ = 0;
    std::ostringstream output_;

    void printStatement(const ast::Stmt* stmt);
    void printExpression(const ast::Expr* expr);
    void printFunction(const ast::FunctionDecl* func);
    void printStruct(const ast::StructDecl* struct_decl);

    // Helpers
    void indent();
    void newline();
    void space();
    bool needsWrapping(const std::string& text);
};
```

### Pretty-Printing Algorithm

#### Step 1: Parse Code

```cpp
std::string Formatter::formatString(const std::string& code) {
    // 1. Parse code to AST
    Lexer lexer(code);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto program = parser.parseProgram();

    // 2. Pretty-print AST
    Printer printer(config_);
    std::string formatted = printer.print(program.get());

    return formatted;
}
```

#### Step 2: Walk AST

```cpp
std::string Printer::print(const ast::Program* program) {
    for (const auto& decl : program->declarations) {
        if (auto* func = dynamic_cast<FunctionDecl*>(decl.get())) {
            printFunction(func);
        } else if (auto* struct_decl = dynamic_cast<StructDecl*>(decl.get())) {
            printStruct(struct_decl);
        }
        // ... other declarations

        newline();  // Blank line between declarations
    }

    return output_.str();
}
```

#### Step 3: Print Each Node

```cpp
void Printer::printFunction(const ast::FunctionDecl* func) {
    // Function signature
    output_ << "function " << func->getName() << "(";

    // Parameters
    const auto& params = func->getParams();
    if (needsWrapping(formatParams(params))) {
        // Multi-line
        newline();
        indent_level_++;
        for (size_t i = 0; i < params.size(); ++i) {
            indent();
            output_ << params[i].name << ": " << params[i].type.toString();
            if (i < params.size() - 1) {
                output_ << ",";
            }
            newline();
        }
        indent_level_--;
        indent();
        output_ << ")";
    } else {
        // Single line
        for (size_t i = 0; i < params.size(); ++i) {
            output_ << params[i].name << ": " << params[i].type.toString();
            if (i < params.size() - 1) {
                output_ << ", ";
            }
        }
        output_ << ")";
    }

    // Return type
    if (func->getReturnType().kind != TypeKind::Void) {
        output_ << " -> " << func->getReturnType().toString();
    }

    // Body
    output_ << " {";
    newline();
    indent_level_++;
    printStatement(func->getBody());
    indent_level_--;
    indent();
    output_ << "}";
    newline();
}
```

---

## CLI Interface

### Commands

```bash
# Format file in-place
naab-fmt file.naab

# Format multiple files
naab-fmt file1.naab file2.naab

# Format entire directory
naab-fmt src/

# Format stdin (for editor integration)
cat file.naab | naab-fmt

# Check if file is formatted (CI mode)
naab-fmt --check file.naab  # Exit code 0 if formatted, 1 if not

# Show diff instead of formatting
naab-fmt --diff file.naab

# Print formatted to stdout (don't modify file)
naab-fmt --print file.naab
```

### Options

```bash
--indent <n>           # Indentation width (2 or 4, default: 4)
--line-length <n>      # Max line length (default: 100)
--config <path>        # Path to .naabfmt config file
--check                # Check if formatted (CI mode)
--diff                 # Show diff
--print                # Print to stdout
--help                 # Show help
--version              # Show version
```

---

## Configuration File

### .naabfmt (Optional)

```json
{
  "indentWidth": 4,
  "maxLineLength": 100,
  "trailingCommas": true,
  "semicolons": "never"
}
```

**Search Order:**
1. `.naabfmt` in current directory
2. `.naabfmt` in parent directories (up to project root)
3. `~/.naabfmt` (global config)
4. Default config

---

## Editor Integration

### VS Code

**Format on Save:**

```json
// settings.json
{
  "[naab]": {
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "naab.naab-language"
  }
}
```

**Extension Integration:**

```typescript
// extension.ts
import * as vscode from 'vscode';
import { exec } from 'child_process';

export function activate(context: vscode.ExtensionContext) {
    let formatter = vscode.languages.registerDocumentFormattingEditProvider('naab', {
        provideDocumentFormattingEdits(document: vscode.TextDocument): vscode.TextEdit[] {
            // Call naab-fmt
            const formatted = execSync('naab-fmt --print', { input: document.getText() });

            // Return edit that replaces entire document
            const fullRange = new vscode.Range(
                document.positionAt(0),
                document.positionAt(document.getText().length)
            );

            return [vscode.TextEdit.replace(fullRange, formatted.toString())];
        }
    });

    context.subscriptions.push(formatter);
}
```

### Neovim

```lua
-- Format on save
vim.api.nvim_create_autocmd("BufWritePre", {
  pattern = "*.naab",
  callback = function()
    vim.cmd("silent !naab-fmt %")
  end,
})

-- Manual format command
vim.api.nvim_create_user_command("NaabFormat", function()
  vim.cmd("silent !naab-fmt %")
  vim.cmd("edit!")
end, {})
```

---

## CI Integration

### GitHub Actions

```yaml
# .github/workflows/format-check.yml
name: Format Check

on: [push, pull_request]

jobs:
  format:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2

      - name: Install naab-fmt
        run: |
          curl -L https://github.com/naab/naab/releases/download/v1.0.0/naab-fmt -o /usr/local/bin/naab-fmt
          chmod +x /usr/local/bin/naab-fmt

      - name: Check formatting
        run: naab-fmt --check src/**/*.naab
```

### Pre-commit Hook

```bash
#!/bin/bash
# .git/hooks/pre-commit

# Format all staged .naab files
for file in $(git diff --cached --name-only --diff-filter=ACM | grep '\.naab$'); do
    naab-fmt "$file"
    git add "$file"
done
```

---

## Testing Strategy

### Unit Tests

**Test Cases:**

1. **Idempotency**: Format twice, same result
   ```cpp
   TEST(FormatterTest, Idempotent) {
       std::string code = "function foo(){return 42}";
       std::string formatted1 = formatter.formatString(code);
       std::string formatted2 = formatter.formatString(formatted1);
       ASSERT_EQ(formatted1, formatted2);
   }
   ```

2. **Semantics Preservation**: Parse before/after, ASTs equal
   ```cpp
   TEST(FormatterTest, PreservesSemantics) {
       std::string original = "let x=42;";
       std::string formatted = formatter.formatString(original);

       auto ast1 = parse(original);
       auto ast2 = parse(formatted);

       ASSERT_TRUE(astEqual(ast1.get(), ast2.get()));
   }
   ```

3. **Line Length**: Respects max line length
   ```cpp
   TEST(FormatterTest, RespectsLineLength) {
       std::string code = "function veryLongFunctionName(param1: string, param2: int, param3: bool) -> int { return 42 }";
       std::string formatted = formatter.formatString(code);

       for (const auto& line : split(formatted, '\n')) {
           ASSERT_LE(line.length(), 100);
       }
   }
   ```

4. **Style Rules**: Each style rule has tests
   - Indentation
   - Braces
   - Semicolons
   - Whitespace
   - etc.

### Integration Tests

**Golden Files:** Compare formatted output to expected output

```
tests/formatter/
├── input/
│   ├── function.naab
│   ├── struct.naab
│   └── complex.naab
└── expected/
    ├── function.naab
    ├── struct.naab
    └── complex.naab
```

```cpp
TEST(FormatterTest, GoldenFiles) {
    for (const auto& test_file : listFiles("tests/formatter/input/")) {
        std::string input = readFile(test_file);
        std::string expected = readFile(getExpectedPath(test_file));
        std::string formatted = formatter.formatString(input);
        ASSERT_EQ(formatted, expected);
    }
}
```

---

## Implementation Plan

### Week 1: Core Formatter (5 days)

- [ ] Set up naab-fmt project structure
- [ ] Implement Printer (basic AST pretty-printing)
- [ ] Implement StyleConfig
- [ ] Implement CLI (format file in-place)
- [ ] Test: Format simple programs correctly

### Week 2: Style Rules & Testing (5 days)

- [ ] Implement all style rules
- [ ] Add line wrapping logic
- [ ] Create comprehensive test suite
- [ ] CI integration example
- [ ] Documentation

**Total: 2 weeks**

---

## Performance

**Target:**
- Format 1000-line file in <100ms
- Format 10,000-line file in <1s

**Optimization:**
- Minimize string copies
- Use `std::ostringstream` for output
- Avoid unnecessary AST traversals

---

## Comparison with Other Formatters

| Feature | naab-fmt | gofmt | rustfmt | Prettier |
|---------|----------|-------|---------|----------|
| Speed | Fast | Very Fast | Fast | Medium |
| Configurable | Minimal | None | Some | Minimal |
| Opinionated | Yes | Yes | Yes | Yes |
| Editor Integration | Yes | Yes | Yes | Yes |
| Preserves Semantics | Yes | Yes | Yes | Yes |

---

## Success Metrics

### Phase 4.2 Complete When:

- [x] naab-fmt formats all NAAb code correctly
- [x] All style rules implemented and tested
- [x] Idempotent (format twice = same result)
- [x] Preserves semantics (no behavior change)
- [x] VS Code integration working
- [x] CI example provided
- [x] Performance acceptable (<100ms for typical file)
- [x] Documentation complete

---

## Conclusion

**Phase 4.2 Status: DESIGN COMPLETE**

An automatic code formatter will:
- Eliminate style debates
- Ensure consistent code across projects
- Reduce code review friction
- Improve code readability

**Implementation Effort:** 2 weeks

**Priority:** Medium (important but not critical)

**Dependencies:** Parser with source location tracking

Once implemented, all NAAb code will have consistent, professional formatting by default.
