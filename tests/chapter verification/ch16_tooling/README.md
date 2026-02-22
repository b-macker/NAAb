# Chapter 16: Tooling Verification Tests

This directory contains verification tests for the NAAb development tooling covered in Chapter 16.

## Test Files

### 1. `formatter_test.naab`

Tests the `naab-fmt` auto-formatter with various code styles.

**Run:**
```bash
# Format the file
naab-lang fmt formatter_test.naab

# Check if formatted (CI mode)
naab-lang fmt --check formatter_test.naab

# Show differences
naab-lang fmt --diff formatter_test.naab

# Run the formatted code
naab-lang run formatter_test.naab
```

**Tests:**
- Struct literal formatting
- Function parameter wrapping
- Dictionary literal formatting
- Multi-line expressions
- Consistent indentation
- Idempotency (format twice = same result)

### 2. `debugger_test.naab`

Tests the interactive debugger with breakpoints and variable inspection.

**Run:**
```bash
# Launch in debug mode
naab-lang run --debug debugger_test.naab

# With watch expressions
naab-lang run --debug --watch="sum,count" debugger_test.naab
```

**Breakpoint Commands:**
When the debugger pauses at a `// breakpoint`:
- `c` - Continue to next breakpoint
- `s` - Step into function
- `n` - Step over function
- `v` - View all variables
- `p a` - Print variable 'a'
- `p a + b` - Evaluate expression
- `w temp` - Watch variable 'temp'
- `h` - Help
- `q` - Quit

**Tests:**
- Breakpoints in loops
- Breakpoints in recursive functions
- Variable inspection
- Expression evaluation
- Watch expressions
- Step-through execution

### 3. `quality_hints_test.naab`

Demonstrates code quality hints across five categories.

**Run:**
```bash
naab-lang run quality_hints_test.naab
```

**Expected Hints:**
- **Performance**: Inefficient array concatenation in loop (O(n²))
- **Best Practices**: Functions too long, too many parameters, empty catch blocks
- **Security**: (would detect SQL injection, XSS if present)
- **Maintainability**: Magic numbers, deep nesting, code duplication
- **Readability**: Complex boolean conditions, long parameter lists

**Categories Tested:**
1. Performance warnings (inefficient patterns)
2. Best practice violations (long functions, empty catch)
3. Maintainability issues (magic numbers, deep nesting)
4. Readability problems (complex conditions)

### 4. `error_hints_test.naab`

Demonstrates enhanced error messages with contextual hints.

**Run:**
```bash
naab-lang run error_hints_test.naab
```

**Demonstrates:**
- ✅ Correct dictionary usage (quoted keys)
- ✅ Struct vs dictionary usage
- ✅ Proper error handling
- ✅ Null safety patterns
- ✅ Correct polyglot block syntax
- ✅ Proper module imports with `use`
- ✅ Variable naming to avoid typos

**Try Breaking It:**
To see error hints in action, introduce these errors:

1. **Unquoted dictionary keys:**
   ```naab
   let person = {
       name: "Alice"  // Remove quotes
   }
   ```
   Error will suggest: "Dictionary keys must be quoted strings"

2. **Wrong import syntax:**
   ```naab
   import io from "std"  // Use JavaScript syntax
   ```
   Error will suggest: "NAAb uses 'use' for imports, not 'import'"

3. **Missing polyglot variable list:**
   ```naab
   let mean = <<python
   statistics.mean(data)  // Forgot [data]
   >>
   ```
   Error will suggest: "Pass variables in brackets: <<python[data]"

4. **Variable name typo:**
   ```naab
   let configuration = {}
   print(confg["timeout"])  // Typo: confg vs configuration
   ```
   Error will suggest: "Did you mean 'configuration'?"

## Verification Checklist

### Formatter (`naab-fmt`)
- [ ] Formats file without errors
- [ ] Idempotent (format twice produces same result)
- [ ] Preserves semantics (program behavior unchanged)
- [ ] Handles struct literals correctly
- [ ] Wraps long function parameters
- [ ] Formats dictionaries consistently
- [ ] Preserves polyglot block content

### Debugger
- [ ] Launches with `--debug` flag
- [ ] Pauses at `// breakpoint` comments
- [ ] `continue` command works
- [ ] `step` command works
- [ ] `next` command works
- [ ] `vars` shows all local variables
- [ ] `print <expr>` evaluates expressions
- [ ] `watch` expressions update automatically
- [ ] Stack traces show correct call hierarchy

### Quality Hints
- [ ] Detects inefficient array concatenation
- [ ] Warns about functions >50 lines
- [ ] Warns about >5 parameters
- [ ] Detects empty catch blocks
- [ ] Identifies magic numbers
- [ ] Warns about deep nesting (>4 levels)
- [ ] Flags complex boolean conditions

### Enhanced Error Messages
- [ ] Parser errors include helpful hints
- [ ] Suggests fixes for common mistakes
- [ ] Shows code examples in errors
- [ ] Suggests similar variable names for typos
- [ ] Includes source context with highlighting
- [ ] Runtime errors include stack traces with local variables

## Configuration

Test with custom formatter config by creating `.naabfmt.toml`:

```toml
[style]
indent_width = 2
max_line_length = 80
semicolons = "always"
trailing_commas = true

[braces]
function_brace_style = "next_line"
```

Then run:
```bash
naab-lang fmt --config .naabfmt.toml formatter_test.naab
```

## Expected Output

All test files should run successfully when executed normally:

```bash
$ naab-lang run formatter_test.naab
Testing formatter...
Name: Alice
Sum: 15
Result: Success: test
Formatter test complete!

$ naab-lang run error_hints_test.naab
Enhanced Error Messages Test
============================
✅ Correct dictionary with quoted keys
...
All examples use correct patterns!
```

## See Also

- `docs/FORMATTER_GUIDE.md` - Complete formatter documentation
- `docs/DEBUGGING_GUIDE.md` - Debugging techniques and tips
- `docs/LLM_BEST_PRACTICES.md` - Guide for generating correct NAAb code
- Chapter 16: Tooling and the Development Environment
- Chapter 12: Error Handling
