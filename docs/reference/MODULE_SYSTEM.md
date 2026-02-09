# Relative Imports in NAAb

**Module System Feature Guide**
**Status:** ✅ Fully Supported (Dot Notation)

---

## Overview

NAAb's module system **fully supports relative imports** using **dot notation** (similar to Python), not slash notation. This allows organizing code in subdirectories and importing modules from nested paths.

---

## Syntax

### Dot Notation (✅ Correct)

```naab
// Import from subdirectory
use utils.helper

// Import from nested subdirectory
use lib.math.calculator

// Import with alias
use lib.math.calculator as calc

// Import from deeply nested structure
use app.services.database.connection
```

### Slash Notation (❌ Not Supported)

```naab
// This does NOT work
use utils/helper        // ❌ Error
use lib/math/calculator // ❌ Error
```

---

## How It Works

The module system converts **dot notation to directory separators** before adding the `.naab` extension.

### Conversion Rules

| Module Path | File Path |
|-------------|-----------|
| `utils.helper` | `utils/helper.naab` |
| `lib.math.calculator` | `lib/math/calculator.naab` |
| `app.services.db` | `app/services/db.naab` |
| `data.models.user` | `data/models/user.naab` |

**Implementation:** `src/runtime/module_system.cpp` lines 32-49

```cpp
// Convert module path to file path
std::string ModuleRegistry::modulePathToFilePath(const std::string& module_path) const {
    std::string file_path = module_path;

    // Replace dots with directory separators
    for (char& c : file_path) {
        if (c == '.') {
            c = std::filesystem::path::preferred_separator;
        }
    }

    // Add .naab extension
    file_path += ".naab";

    return file_path;
}
```

---

## Examples

### Example 1: Simple Subdirectory

**File Structure:**
```
myproject/
├── main.naab
└── utils/
    └── helper.naab
```

**utils/helper.naab:**
```naab
export fn greet(name: string) -> string {
    return "Hello, " + name
}
```

**main.naab:**
```naab
use utils.helper

main {
    print(helper.greet("World"))
    // Output: Hello, World
}
```

---

### Example 2: Nested Subdirectories

**File Structure:**
```
project/
├── main.naab
└── lib/
    └── math/
        └── calculator.naab
```

**lib/math/calculator.naab:**
```naab
export fn add(a: int, b: int) -> int {
    return a + b
}

export fn multiply(a: int, b: int) -> int {
    return a * b
}
```

**main.naab:**
```naab
use lib.math.calculator

main {
    print("5 + 3 =", calculator.add(5, 3))
    print("5 * 3 =", calculator.multiply(5, 3))
}
```

**Output:**
```
5 + 3 = 8
5 * 3 = 15
```

---

### Example 3: With Alias

**main.naab:**
```naab
use lib.math.calculator as calc

main {
    print("Using alias 'calc':")
    print("  10 + 20 =", calc.add(10, 20))
    print("  10 * 20 =", calc.multiply(10, 20))
}
```

**Output:**
```
Using alias 'calc':
  10 + 20 = 30
  10 * 20 = 200
```

---

### Example 4: Multiple Levels

**File Structure:**
```
app/
├── main.naab
├── models/
│   └── user.naab
├── services/
│   ├── auth.naab
│   └── database/
│       └── connection.naab
└── utils/
    ├── string.naab
    └── validation.naab
```

**main.naab:**
```naab
use models.user
use services.auth
use services.database.connection as db
use utils.string
use utils.validation

main {
    // All modules loaded with dot notation
    print("App initialized")
}
```

---

## Module Resolution

The module system searches for modules in the following order:

### 1. Relative to Current Directory

```naab
// From: /home/project/main.naab
use utils.helper

// Searches: /home/project/utils/helper.naab ✅
```

### 2. Search Paths (if configured)

```naab
// Can add search paths via NAAB_PATH environment variable (future)
```

### 3. Standard Library

```naab
// Built-in modules (no file search)
use io
use math
use string
```

---

## Best Practices

### ✅ DO

1. **Use dot notation for all relative imports**
   ```naab
   use lib.math.calculator
   use app.services.auth
   ```

2. **Use aliases for long module names**
   ```naab
   use very.long.module.path.name as short
   ```

3. **Organize code in logical directories**
   ```
   app/
   ├── models/
   ├── services/
   ├── utils/
   └── main.naab
   ```

4. **Export only what's needed**
   ```naab
   export fn publicFunction() -> int { ... }

   fn privateHelper() -> int { ... }  // Not exported
   ```

### ❌ DON'T

1. **Don't use slash notation**
   ```naab
   use lib/math/calculator  // ❌ Won't work
   ```

2. **Don't include .naab extension**
   ```naab
   use utils.helper.naab  // ❌ Wrong (searches for utils/helper/naab.naab)
   ```

3. **Don't use absolute paths**
   ```naab
   use /home/user/project/utils.helper  // ❌ Not supported
   ```

4. **Don't create circular dependencies**
   ```naab
   // a.naab
   use b

   // b.naab
   use a  // ❌ Circular dependency
   ```

---

## Common Patterns

### Pattern 1: Utility Module Organization

```
project/
├── main.naab
└── utils/
    ├── string.naab
    ├── array.naab
    ├── math.naab
    └── validation.naab
```

```naab
use utils.string as str
use utils.array as arr
use utils.math
use utils.validation as validate
```

---

### Pattern 2: Feature-Based Organization

```
app/
├── main.naab
├── users/
│   ├── model.naab
│   ├── service.naab
│   └── controller.naab
└── posts/
    ├── model.naab
    ├── service.naab
    └── controller.naab
```

```naab
use users.model as UserModel
use users.service as UserService
use posts.model as PostModel
use posts.service as PostService
```

---

### Pattern 3: Shared Libraries

```
project/
├── main.naab
├── lib/
│   ├── database/
│   │   ├── connection.naab
│   │   └── query.naab
│   └── http/
│       ├── client.naab
│       └── server.naab
```

```naab
use lib.database.connection as db
use lib.database.query
use lib.http.client
```

---

## Comparison with Other Languages

### Python

```python
# Python uses dot notation (similar to NAAb)
from lib.math import calculator
from lib.math.calculator import add

# Also supports relative imports with dots
from .utils import helper
from ..models import user
```

**NAAb equivalent:**
```naab
// NAAb uses dot notation without 'from'
use lib.math.calculator
// Note: NAAb doesn't support relative-to-current-module syntax yet
```

---

### JavaScript/TypeScript

```javascript
// JavaScript uses slash notation
import { calculator } from './lib/math/calculator';
import helper from '../utils/helper';
```

**NAAb equivalent:**
```naab
// NAAb uses dot notation instead of slashes
use lib.math.calculator
// Note: No concept of ./ or ../ - always from project root
```

---

### Rust

```rust
// Rust uses module tree syntax
use lib::math::calculator;
use crate::utils::helper;
```

**NAAb equivalent:**
```naab
// NAAb is very similar to Rust
use lib.math.calculator
use utils.helper
// No need for 'crate::' - always from project root
```

---

## Troubleshooting

### Error: "Failed to load module: utils"

**Problem:**
```naab
use utils/helper  // ❌ Using slash notation
```

**Solution:**
```naab
use utils.helper  // ✅ Use dot notation
```

---

### Error: "Module 'utils.helper' has no member 'greet'"

**Problem:** The module hasn't exported the function

**Solution:**
```naab
// utils/helper.naab
export fn greet(name: string) -> string {  // ✅ Add 'export'
    return "Hello, " + name
}
```

---

### Module not found in nested directory

**Problem:** File structure doesn't match module path

**File structure:**
```
lib/
└── math.naab  // ❌ Wrong location
```

**Using:**
```naab
use lib.math.calculator  // Expects lib/math/calculator.naab
```

**Solution:** Match file structure to module path
```
lib/
└── math/
    └── calculator.naab  // ✅ Correct location
```

---

## Testing

### Test 1: Basic Relative Import

```bash
mkdir -p testproject/utils
cat > testproject/utils/helper.naab << 'EOF'
export fn greet(name: string) -> string {
    return "Hello, " + name
}
EOF

cat > testproject/main.naab << 'EOF'
use utils.helper

main {
    print(helper.greet("World"))
}
EOF

cd testproject && naab-lang run main.naab
# Output: Hello, World
```

---

### Test 2: Nested Import

```bash
mkdir -p testproject/lib/math
cat > testproject/lib/math/calculator.naab << 'EOF'
export fn add(a: int, b: int) -> int {
    return a + b
}
EOF

cat > testproject/main.naab << 'EOF'
use lib.math.calculator

main {
    print("5 + 3 =", calculator.add(5, 3))
}
EOF

cd testproject && naab-lang run main.naab
# Output: 5 + 3 = 8
```

---

### Test 3: With Alias

```bash
cat > testproject/main.naab << 'EOF'
use lib.math.calculator as calc

main {
    print("Result:", calc.add(10, 20))
}
EOF

cd testproject && naab-lang run main.naab
# Output: Result: 30
```

---

## Implementation Details

### Module Path Resolution

1. **Parse:** `use lib.math.calculator`
2. **Convert:** Replace `.` with `/` → `lib/math/calculator`
3. **Append:** Add `.naab` → `lib/math/calculator.naab`
4. **Resolve:** Search from current directory
5. **Load:** Parse and execute module
6. **Register:** Add to module registry with original path as key

### Key Functions

**File:** `src/runtime/module_system.cpp`

```cpp
// Convert module path to file path
std::string modulePathToFilePath(const std::string& module_path)

// Resolve module path to absolute file path
std::optional<std::string> resolveModulePath(
    const std::string& module_path,
    const std::filesystem::path& current_dir
)

// Load a module
NaabModule* loadModule(
    const std::string& module_path,
    const std::filesystem::path& current_dir
)
```

---

## Future Enhancements

### Potential Additions

1. **Relative imports with ./  and ../**
   ```naab
   use ./helper      // Current directory
   use ../utils/helper  // Parent directory
   ```

2. **Wildcard imports**
   ```naab
   use utils.*       // Import all from utils
   ```

3. **Selective imports**
   ```naab
   use utils.helper { greet, farewell }  // Import specific items
   ```

4. **Module search paths configuration**
   ```bash
   export NAAB_PATH="/usr/local/naab/modules:/opt/naab/lib"
   ```

---

## References

- **Implementation:** `src/runtime/module_system.cpp` lines 32-76
- **Parser:** `src/parser/parser.cpp` (use statement parsing)
- **Interpreter:** `src/interpreter/interpreter.cpp` (module loading)

---

## Changelog

### Version 1.0 (2026-01-31)

**Documentation Created:**
- ✅ Relative imports fully documented
- ✅ Dot notation syntax explained
- ✅ Examples and patterns provided
- ✅ Common errors and solutions documented
- ✅ ISS-035 resolved (not a missing feature)

**Feature Status:**
- Dot notation: ✅ Supported
- Nested directories: ✅ Supported
- Aliases: ✅ Supported
- Module resolution: ✅ Working

---

**Document Version:** 1.0
**Last Updated:** 2026-01-31
**Status:** ✅ FEATURE DOCUMENTED

**Relative Imports:** ✅ **FULLY SUPPORTED** (Dot Notation)
