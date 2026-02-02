# NAAb Auto-Formatter Guide

The NAAb auto-formatter (`naab-fmt`) ensures consistent code style across all NAAb projects.

## Quick Start

Format a single file in-place:
```bash
naab-fmt myfile.naab
```

Check if a file is formatted (CI mode):
```bash
naab-fmt --check myfile.naab
```

Format with custom config:
```bash
naab-fmt --config=custom.toml myfile.naab
```

## Installation

The formatter is included with `naab-lang`:
```bash
naab-lang fmt <file.naab>
```

## Usage

### Basic Formatting

Format files in-place:
```bash
naab-fmt file1.naab file2.naab
```

### Check Mode

Verify formatting without modifying files (useful for CI):
```bash
naab-fmt --check src/**/*.naab
```

Exit codes:
- `0`: All files are properly formatted
- `1`: One or more files need formatting

### Diff Mode

Show what would change:
```bash
naab-fmt --check --diff file.naab
```

## Configuration

Create a `.naabfmt.toml` file in your project root:

```toml
[style]
indent_width = 4              # 2 or 4 spaces
max_line_length = 100         # 80-120 characters
semicolons = "never"          # "always", "never", "as-needed"
trailing_commas = true        # Add trailing commas in multi-line

[braces]
function_brace_style = "same_line"        # "same_line" or "next_line"
control_flow_brace_style = "same_line"

[spacing]
blank_lines_between_declarations = 1
blank_lines_between_sections = 2
space_before_function_paren = false
space_in_empty_parens = false

[wrapping]
wrap_function_params = "auto"    # "auto", "always", "never"
wrap_struct_fields = "auto"
wrap_array_elements = "auto"
align_wrapped_params = true
```

### Style Options

#### `indent_width`
Number of spaces per indentation level (2 or 4).

**Default:** `4`

```naab
fn example() {
    let x = 42    // 4-space indent
    let y = 10
}
```

#### `max_line_length`
Maximum characters per line before wrapping.

**Default:** `100`

#### `semicolons`
When to add semicolons:
- `"always"`: Add semicolons after every statement
- `"never"`: Never add semicolons (NAAb's default)
- `"as-needed"`: Only when multiple statements on one line

**Default:** `"never"`

#### `trailing_commas`
Add trailing commas in multi-line lists.

**Default:** `true`

```naab
let data = [
    1,
    2,
    3,  // ✓ Trailing comma
]
```

### Brace Styles

#### `function_brace_style`
- `"same_line"`: K&R/Egyptian style (default)
- `"next_line"`: Allman style

```naab
// same_line (default)
fn example() {
    // body
}

// next_line
fn example()
{
    // body
}
```

### Wrapping Options

#### `wrap_function_params`
When to wrap function parameters:
- `"auto"`: Wrap if line too long (default)
- `"always"`: Always wrap (one per line)
- `"never"`: Never wrap

```naab
// auto - wraps when needed
fn longFunction(param1: int, param2: string, param3: bool) {
    // ...
}

// always
fn longFunction(
    param1: int,
    param2: string,
    param3: bool
) {
    // ...
}
```

## Formatting Rules

### Functions

```naab
// ✓ Formatted
fn calculate(x: int, y: int) -> int {
    return x + y
}

// ✗ Before formatting
fn calculate(x:int,y:int)->int{return x+y}
```

### Structs

```naab
// ✓ Multi-line when many fields
struct Person {
    name: string,
    age: int,
    email: string,
}

// ✓ Single-line when few fields
struct Point { x: int, y: int }
```

### Arrays and Dictionaries

```naab
// ✓ Multi-line when long
let data = [
    1, 2, 3, 4, 5,
    6, 7, 8, 9, 10,
]

// ✓ Single-line when short
let coords = [1, 2, 3]

// ✓ Dictionary with quoted keys
let person = {
    "name": "Alice",
    "age": 30,
}
```

### Control Flow

```naab
// ✓ Consistent bracing
if condition {
    doSomething()
} else {
    doSomethingElse()
}

// ✓ Single-line when simple
if x > 0 { return x }
```

## Editor Integration

### VS Code

Add to `.vscode/settings.json`:
```json
{
  "[naab]": {
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "naab.naab-fmt"
  }
}
```

### Vim/Neovim

```vim
autocmd BufWritePre *.naab !naab-fmt %
```

## CI/CD Integration

### GitHub Actions

```yaml
- name: Check formatting
  run: naab-fmt --check src/**/*.naab
```

### Pre-commit Hook

Create `.git/hooks/pre-commit`:
```bash
#!/bin/bash
naab-fmt --check $(git diff --cached --name-only --diff-filter=ACM | grep '\.naab$')
```

## Idempotence

The formatter is idempotent - formatting twice produces the same result:
```bash
naab-fmt file.naab
naab-fmt file.naab  # No changes on second run
```

## Preserving Semantics

The formatter **never** changes program behavior:
- ✓ Whitespace and formatting only
- ✓ Comments are preserved
- ✗ Never modifies logic or structure

## Examples

See `tests/formatter/` for comprehensive examples:
- `basic_test.naab` - Basic formatting rules
- `edge_cases_test.naab` - Complex scenarios
- `config_test.toml` - Configuration examples

## Troubleshooting

### Formatter produces parse errors

The formatter requires valid NAAb syntax. Fix syntax errors first:
```bash
naab-lang check file.naab  # Check for errors
naab-fmt file.naab          # Then format
```

### Custom config not loading

Ensure `.naabfmt.toml` is in the current directory or specify path:
```bash
naab-fmt --config=path/to/config.toml file.naab
```

### Files not being formatted

Check file permissions and ensure path is correct:
```bash
ls -l file.naab
naab-fmt --verbose file.naab
```

## Performance

The formatter is optimized for speed:
- **Typical speed:** <100ms per file
- **Large files (1000+ lines):** <500ms

## See Also

- [Style Guide](STYLE_GUIDE.md) - Official NAAb style conventions
- [Developer Guide](DEVELOPER_GUIDE.md) - Contributing to the formatter
