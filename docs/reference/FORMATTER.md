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
naab-fmt --diff file.naab
```
**Note:** The `--diff` flag now automatically implies `--check`, meaning it will show differences without modifying the file.



## Configuration

**Note:** Custom configuration via `.naabfmt.toml` files is currently **unimplemented**. The formatter operates with its default settings.

Future plans include supporting configuration files (e.g., `.naabfmt.toml`) to customize formatting rules like indentation width, line length, and brace styles.


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
