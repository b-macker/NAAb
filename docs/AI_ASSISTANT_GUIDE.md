# AI Assistant Guide for NAAb Language

**Audience**: AI assistants (Claude, GPT, Gemini, etc.) helping users with NAAb
**Purpose**: Complete reference for understanding and supporting NAAb development
**Last Updated**: January 20, 2026
**Version**: 3.0 (Phase 2 Complete Edition)
**Project Status**: 79% Production Ready (Phase 2: 100% ✅ | Phase 3: 67% ⚠️ | Phase 5: 100% ✅)

---

## 🚀 Quick Start for AI Assistants

### What You Need to Know RIGHT NOW

**NAAb is a polyglot programming language** that:
- Embeds 8 languages (Python, JavaScript, C++, Bash, Rust, Ruby, Go, C#)
- Has a **complete type system** (generics, union types, type inference, null safety)
- Has **production-ready error handling** (try/catch/throw, stack traces)
- Has **automatic garbage collection** (tracing GC with cycle detection)
- Has **13 native stdlib modules** (10-100x faster than polyglot)
- **NEW (2026-01-20):** Array element assignment (`arr[i] = value`)

### Current Project Status (2026-01-20)

| Component | Status | Completion |
|-----------|--------|------------|
| **Phase 1: Parser** | ✅ Complete | 100% |
| **Phase 2: Type System** | ✅ Complete | 100% |
| **Phase 3: Runtime** | ⚠️ Partial | 67% |
| **Phase 4: Tooling** | ⚠️ Design Only | 0% |
| **Phase 5: Stdlib** | ✅ Complete | 100% |
| **Overall** | ⚠️ In Progress | **79%** |

### What Just Got Completed (Last 8 Days)

**2026-01-20 (TODAY):**
- ✅ **Array Element Assignment** - `arr[i] = value`, `dict[key] = value` now works!
- ✅ Sorting algorithms unblocked (bubble sort tested with 10-200 elements)
- ✅ All in-place algorithms now possible (matrices, graphs, etc.)

**2026-01-19:**
- ✅ Complete tracing garbage collector with cycle detection
- ✅ Automatic GC triggering (threshold-based)
- ✅ Enhanced error messages with source locations
- ✅ Benchmarking suite (4 micro + 2 macro benchmarks)

**2026-01-18:**
- ✅ Type inference (variables, function returns, generic arguments)
- ✅ Exception system verification (10/10 tests passing)
- ✅ Memory model analysis completed

**2026-01-17:**
- ✅ Generics with monomorphization
- ✅ Union types (`int | string`)
- ✅ Null safety (`int?` syntax)
- ✅ Standard library (13 modules, 4000+ lines C++)

---

## 📚 Table of Contents

### Core Language (What Users Need)
1. **NAAb Language Syntax** - Types, operators, control flow, functions
2. **Type System** - Generics, unions, type inference, null safety
3. **Error Handling** - Try/catch/throw, stack traces
4. **Standard Library** - 13 native modules
5. **Polyglot Features** - Embedding Python, JS, etc.

### Development (What You Need to Help)
6. **Project Structure** - Where everything lives
7. **Build System** - How to compile
8. **Testing** - How to run tests
9. **Common Issues** - What breaks and how to fix it
10. **Recent Changes** - What's new this week

### Reference (Quick Lookup)
11. **File Locations** - Complete path reference
12. **Quick Commands** - Essential operations
13. **Status Documents** - Where to find current state
14. **Known Limitations** - What doesn't work yet

---

## 1. NAAb Language Syntax

### Basic Data Types

```naab
# Integers and Floats
let age: int = 25
let price: float = 99.99

# Strings (immutable)
let name: string = "Alice"
let multiline: string = "Hello
World"

# Booleans
let is_active: bool = true

# Lists (dynamic arrays)
let numbers: list = [1, 2, 3, 4, 5]
let mixed: list = [1, "two", 3.0, true]

# Dictionaries (hash maps)
let person: dict = {
    "name": "Alice",
    "age": "30",
    "city": "NYC"
}

# Void (no return value)
fn log_message(msg: string) -> void {
    print(msg)
}

# Type inference (NEW!)
let x = 42              # Inferred as int
let y = 3.14            # Inferred as float
let z = "hello"         # Inferred as string
```

### Array Element Assignment (NEW - 2026-01-20!)

```naab
# List assignment with bounds checking
let arr = [10, 20, 30]
arr[0] = 99             # ✅ Works! Result: [99, 20, 30]
arr[2] = 77             # ✅ Works! Result: [99, 20, 77]

# Dictionary assignment (creates or updates keys)
let person = {"name": "Alice", "age": "30"}
person["age"] = "31"    # ✅ Update existing key
person["city"] = "NYC"  # ✅ Create new key

# Assignment in loops
let data = [0, 0, 0, 0, 0]
let i = 0
while i < 5 {
    data[i] = i * 10
    i = i + 1
}
# Result: [0, 10, 20, 30, 40]

# Expressions as values
let vals = [1, 2, 3]
vals[0] = vals[1] + vals[2]  # vals[0] = 5
```

### Advanced Type System

#### Generics (Monomorphization)

```naab
# Generic struct
struct Box<T> {
    value: T
}

# Generic function
fn identity<T>(x: T) -> T {
    return x
}

# Type inference with generics (NEW!)
let num = identity(42)        # identity<int>(42) inferred
let str = identity("hello")   # identity<string>("hello") inferred

# Manual type specification
let result = identity<float>(3.14)
```

#### Union Types

```naab
# Variable can be int OR string
fn process(value: int | string) -> string {
    if type(value) == "int" {
        return string.from_int(value)
    }
    return value
}

let x = process(42)        # Returns "42"
let y = process("hello")   # Returns "hello"
```

#### Null Safety

```naab
# Non-nullable by default
let name: string = "Alice"   # Cannot be null
# let bad: string = null     # ❌ Error!

# Nullable types with ? suffix
let middle_name: string? = null    # ✅ OK
let age: int? = null               # ✅ OK

# Null checking required
fn get_length(s: string?) -> int {
    if s != null {
        return string.length(s)
    }
    return 0
}
```

#### Enums

```naab
enum Status {
    PENDING,
    ACTIVE,
    COMPLETED
}

let current = Status::ACTIVE
```

### Control Flow

```naab
# If/Else
if x > 10 {
    print("Large")
} else if x > 5 {
    print("Medium")
} else {
    print("Small")
}

# While loops
let i = 0
while i < 10 {
    print(i)
    i = i + 1
}

# For-in loops (lists only - no range operator yet)
let nums = [1, 2, 3, 4, 5]
for num in nums {
    print(num)
}

# NOTE: Range operator (..) not yet implemented
# Must use while loops for numeric ranges
```

### Functions

```naab
# Basic function
fn add(a: int, b: int) -> int {
    return a + b
}

# Return type inference (NEW!)
fn get_number() {      # Return type inferred as int
    return 42
}

# No return value (void)
fn log(msg: string) -> void {
    print(msg)
}

# Multiple returns
fn min_max(a: int, b: int) -> list {
    if a < b {
        return [a, b]
    }
    return [b, a]
}
```

### Structs

```naab
# Define struct
struct Person {
    name: string,
    age: int
}

# Create instance (requires 'new' keyword)
let alice = new Person {
    name: "Alice",
    age: 30
}

# Access fields
print(alice.name)      # "Alice"
print(alice.age)       # 30

# Modify fields
alice.age = 31

# Reference semantics (shared pointers)
let bob = alice        # bob and alice point to same struct
bob.name = "Bob"
print(alice.name)      # "Bob" (shared reference!)
```

### Error Handling

```naab
# Try/catch/finally
fn divide(a: int, b: int) -> int {
    try {
        if b == 0 {
            throw "Division by zero"
        }
        return a / b
    } catch error {
        print("Error: ", error)
        return 0
    } finally {
        print("Cleanup")  # Always executes
    }
}

# Multi-level exception propagation
fn level3() {
    throw "Error at level 3"
}

fn level2() {
    level3()  # Exception propagates up
}

fn level1() {
    try {
        level2()
    } catch error {
        print("Caught: ", error)  # Catches from level3
    }
}

# Stack traces automatically included
# Error messages show source location (line:column)
```

### Polyglot Embedding

#### Inline Code (Basic)

```naab
main {
    # Python
    <<python
    print("Hello from Python!")
    >>

    # JavaScript
    <<javascript
    console.log("Hello from JavaScript!");
    >>

    # Shell/Bash
    <<shell
    echo "Hello from Shell!"
    >>

    # C++ (compiled at runtime)
    <<cpp
    std::cout << "Hello from C++!" << std::endl;
    >>

    # Also supported: rust, ruby, go, csharp
}
```

#### Variable Binding (Pass NAAb variables to embedded code)

```naab
main {
    let name = "Alice"
    let age = 30
    let price = 99.99

    # Bind variables with [var1, var2] syntax
    <<python[name, age, price]
    print(f"Name: {name}, Age: {age}, Price: {price}")
    >>

    # JavaScript
    <<javascript[name, age]
    console.log(`Name: ${name}, Age: ${age}`);
    >>

    # All basic types supported: int, float, string, bool, list, dict
}
```

#### Return Values (Get results from embedded code)

```naab
main {
    # Return single value (last expression)
    let result = <<python
    2 + 2
    >>
    print("Python result: ", result)  # 4

    # Return from JavaScript
    let sum = <<javascript
    [1, 2, 3, 4, 5].reduce((a, b) => a + b, 0)
    >>
    print("JS sum: ", sum)  # 15

    # Return complex types
    let data = <<python
    {"name": "Alice", "scores": [95, 87, 92]}
    >>
    print(data["name"])  # "Alice"

    # Types supported: int, float, string, bool, list, dict, void
}
```

---

## 2. Standard Library (13 Modules - 100% Complete)

### Module: array (List Operations)

```naab
use array as arr

main {
    let nums = [3, 1, 4, 1, 5, 9, 2, 6]

    # Length
    let len = arr.length(nums)  # 8

    # Add element
    let new_list = arr.push(nums, 100)

    # Map, filter, reduce
    let doubled = arr.map(nums, fn(x) { return x * 2 })
    let evens = arr.filter(nums, fn(x) { return x % 2 == 0 })
    let sum = arr.reduce(nums, fn(acc, x) { return acc + x }, 0)

    # Other utilities
    let reversed = arr.reverse(nums)
    let sorted = arr.sort(nums)
    let contains = arr.contains(nums, 5)  # true
}
```

### Module: string (String Operations)

```naab
use string as str

main {
    let text = "  Hello World  "

    # 14 functions available
    let upper = str.upper(text)           # "  HELLO WORLD  "
    let lower = str.lower(text)           # "  hello world  "
    let trimmed = str.trim(text)          # "Hello World"
    let len = str.length(text)            # 15
    let parts = str.split("a,b,c", ",")   # ["a", "b", "c"]
    let joined = str.join(["a", "b"], ",") # "a,b"
    let replaced = str.replace(text, "World", "NAAb")
    let has = str.contains(text, "World")  # true
    let starts = str.starts_with(text, "  Hello")  # true
    let ends = str.ends_with(text, "World  ")      # true
    let substr = str.substring(text, 2, 7) # "Hello"
    let index = str.index_of("hello", "ll")  # 2 (or -1 if not found)
    let repeated = str.repeat("abc", 3)      # "abcabcabc"
    let from_int = str.from_int(42)          # "42"
}
```

### Module: math (Mathematical Functions)

```naab
use math

main {
    # 11 functions + 2 constants
    let a = math.abs(-5)          # 5
    let p = math.pow(2.0, 3.0)    # 8.0
    let s = math.sqrt(16.0)       # 4.0
    let r = math.round(3.7)       # 4.0
    let f = math.floor(3.7)       # 3.0
    let c = math.ceil(3.2)        # 4.0
    let min_val = math.min(5, 3)  # 3
    let max_val = math.max(5, 3)  # 5

    # Trigonometry
    let sine = math.sin(math.PI / 2)    # 1.0
    let cosine = math.cos(0.0)          # 1.0
    let tangent = math.tan(math.PI / 4) # 1.0

    # Constants
    let pi = math.PI      # 3.14159...
    let e = math.E        # 2.71828...
}
```

### Module: json (JSON Parsing & Serialization)

```naab
use json

main {
    # Parse JSON string to dict/list
    let data = json.parse('{"name": "Alice", "age": 30}')
    print(data["name"])  # "Alice"

    # Serialize dict/list to JSON
    let person = {"name": "Bob", "scores": [95, 87, 92]}
    let json_str = json.stringify(person)
    # '{"name":"Bob","scores":[95,87,92]}'

    # Pretty print
    let pretty = json.pretty(person, 2)  # Indented with 2 spaces

    # Validation
    let valid = json.validate('{"key": "value"}')  # true
    let invalid = json.validate('{bad json}')      # false
}
```

### Module: http (HTTP Client)

```naab
use http

main {
    # GET request
    let response = http.get("https://api.example.com/users")
    print(response["status"])   # 200
    print(response["body"])     # Response body

    # POST request
    let data = {"name": "Alice", "email": "alice@example.com"}
    let create_response = http.post("https://api.example.com/users", data)

    # PUT request
    let update_response = http.put("https://api.example.com/users/1", data)

    # DELETE request
    let delete_response = http.delete("https://api.example.com/users/1")

    # Response structure:
    # {
    #   "status": 200,
    #   "body": "...",
    #   "headers": {...}
    # }
}
```

### Module: io (File I/O)

```naab
use io

main {
    # Read entire file
    let content = io.read("file.txt")

    # Write to file (overwrites)
    io.write("output.txt", "Hello World")

    # Append to file
    io.append("log.txt", "New log entry\n")

    # Check if file exists
    let exists = io.exists("file.txt")  # true/false
}
```

### Module: file (Extended File Operations)

```naab
use file

main {
    # List directory
    let files = file.list_dir("/path/to/dir")

    # Create directory
    file.create_dir("/path/to/new_dir")

    # Delete file
    file.delete("file.txt")

    # Copy file
    file.copy("source.txt", "dest.txt")

    # Move/rename file
    file.move("old.txt", "new.txt")
}
```

### Module: time (Time & Date)

```naab
use time

main {
    # Current Unix timestamp (seconds)
    let now = time.now()

    # Current time in milliseconds
    let ms = time.milliseconds()

    # Current time in nanoseconds
    let ns = time.nanoseconds()

    # Format timestamp
    let formatted = time.format(now, "%Y-%m-%d %H:%M:%S")
    # "2026-01-20 14:30:00"

    # Parse time string
    let timestamp = time.parse("2026-01-20", "%Y-%m-%d")

    # Sleep (pause execution)
    time.sleep(1000)  # Sleep for 1000 milliseconds (1 second)
}
```

### Other Modules (Brief Overview)

**env** - Environment variables
```naab
use env
let path = env.get("PATH")
env.set("MY_VAR", "value")
```

**csv** - CSV parsing and writing
```naab
use csv
let data = csv.parse("a,b,c\n1,2,3")
csv.write("output.csv", data)
```

**regex** - Regular expressions
```naab
use regex
let matches = regex.match("hello123", "[0-9]+")  # ["123"]
let replaced = regex.replace("hello123", "[0-9]+", "XXX")
```

**crypto** - Hashing and encryption
```naab
use crypto
let hash = crypto.sha256("password")
let md5 = crypto.md5("data")
```

**collections** - Set operations
```naab
use collections as coll
let my_set = coll.Set()
coll.set_add(my_set, "item1")
coll.set_add(my_set, "item2")
let has = coll.set_contains(my_set, "item1")  # true
let size = coll.set_size(my_set)  # 2
```

---

## 3. Project Structure

### Directory Layout

```
~/.naab/
├── language/               # Main NAAb implementation (C++)
│   ├── src/               # Source code
│   │   ├── lexer/         # Tokenization
│   │   ├── parser/        # AST construction
│   │   ├── interpreter/   # Execution engine
│   │   ├── runtime/       # Type system, GC, stdlib
│   │   └── executors/     # Polyglot language executors
│   ├── include/naab/      # Headers
│   ├── build/             # Build output
│   │   └── naab-lang      # Main executable
│   ├── stdlib/            # Standard library (C++ implementations)
│   ├── examples/          # Test programs
│   ├── benchmarks/        # Performance tests
│   └── docs/              # Documentation
│       └── sessions/      # Development session logs
├── docs/                  # Project-wide documentation
│   ├── AI_ASSISTANT_GUIDE.md      # This file
│   ├── MASTER_STATUS.md           # Current project status
│   └── PRODUCTION_READINESS_PLAN.md  # Implementation plan
└── cli/                   # CLI tools (future)
```

### Key Files

**Status & Planning:**
- `MASTER_STATUS.md` - Current project state (79% complete)
- `PRODUCTION_READINESS_PLAN.md` - Full implementation plan
- `BENCHMARKING_SUMMARY.md` - Benchmark results

**Implementation:**
- `src/interpreter/interpreter.cpp` - Main execution engine
- `src/parser/parser.cpp` - Syntax parsing
- `src/runtime/` - Type system, GC, memory management
- `stdlib/` - 13 native C++ modules

**Testing:**
- `examples/test_*.naab` - Test files
- `benchmarks/` - Performance benchmarks

---

## 4. Build System

### Building NAAb

```bash
cd ~/.naab/language

# Clean build (if needed)
rm -rf build
mkdir build
cd build

# Configure with CMake
cmake ..

# Build (parallel)
make -j$(nproc)

# Verify build
ls -lh naab-lang  # Should be ~40-50MB
```

### Quick Rebuild

```bash
cd ~/.naab/language/build
make naab-lang
```

### Build Targets

- `naab-lang` - Main runtime executable
- `naab-repl` - Interactive REPL (future)
- `libnaab_stdlib.a` - Standard library

---

## 5. Running NAAb Programs

### Basic Execution

```bash
cd ~/.naab/language/build

# Run a program
./naab-lang run ../examples/test_hello.naab

# Run with verbose output (shows debug logs)
./naab-lang run ../examples/test_phase2_4_1_generics.naab
```

### Test Files Available

```bash
# Type system tests
./naab-lang run ../examples/test_simple_inference.naab
./naab-lang run ../examples/test_phase2_4_1_generics.naab
./naab-lang run ../examples/test_phase2_4_2_union_types.naab
./naab-lang run ../examples/test_nullable_simple.naab

# Error handling tests
./naab-lang run ../examples/test_phase3_1_exceptions_final.naab
./naab-lang run ../examples/test_simple_error.naab

# GC tests
./naab-lang run ../examples/test_gc_automatic_intensive.naab
./naab-lang run ../examples/test_gc_with_collection.naab

# Array assignment tests (NEW!)
./naab-lang run ../test_array_assignment.naab
./naab-lang run ../test_array_assignment_complete.naab

# Benchmarks
./naab-lang run ../benchmarks/micro/01_variables_simple.naab
./naab-lang run ../benchmarks/macro/fibonacci.naab
./naab-lang run ../benchmarks/macro/sorting.naab  # NEW - now works!
```

---

## 6. Common Issues & Solutions

### Issue 1: Build Fails

**Symptoms:**
```
error: 'std::string' has not been declared
```

**Solution:**
Missing include. Check that `#include <string>` is in the file.

### Issue 2: Dictionary Key Not Found

**Symptoms:**
```
Error: Dictionary key not found: city
```

**Cause:** Trying to read a key that doesn't exist.

**Solution:**
```naab
# Check key exists first
if dict.contains(person, "city") {
    print(person["city"])
}

# Or use assignment to create key
person["city"] = "NYC"  # Creates key if missing
```

### Issue 3: List Index Out of Bounds

**Symptoms:**
```
Error: List index out of bounds: 5
```

**Solution:**
```naab
let arr = [1, 2, 3]
# arr[5] = 10  # ❌ Error!

# Check bounds first
if index < array.length(arr) {
    arr[index] = value
}
```

### Issue 4: Cannot Assign to Array Element

**Status:** ✅ FIXED (2026-01-20)

If you see old documentation saying array assignment doesn't work, it's outdated. It now works!

```naab
# This works now!
let nums = [1, 2, 3]
nums[0] = 99
nums[2] = 77
# Result: [99, 2, 77]
```

### Issue 5: Type Mismatch

**Symptoms:**
```
Error: Expected int, got string
```

**Solution:**
```naab
# Use type conversions
let str = "42"
let num = string.to_int(str)  # Convert to int

# Or use union types
fn process(value: int | string) -> void {
    # Handle both types
}
```

---

## 7. Known Limitations (As of 2026-01-20)

### ❌ Missing Features (3 Remaining)

1. **Range Operator (..)** - MEDIUM PRIORITY
   ```naab
   # Doesn't work yet:
   # for i in 0..10 { ... }

   # Workaround - use while:
   let i = 0
   while i < 10 {
       print(i)
       i = i + 1
   }
   ```

2. **Time Module for Benchmarking** - HIGH PRIORITY
   ```naab
   # Can't measure performance in code yet
   # Must use external timing:
   # time ./naab-lang run program.naab
   ```

3. **List Member Methods** - LOW PRIORITY
   ```naab
   # Doesn't work yet:
   # let len = my_list.length()

   # Workaround - use array module:
   use array
   let len = array.length(my_list)
   ```

### ⚠️ Known Behaviors

1. **Struct literals require `new` keyword:**
   ```naab
   # Required:
   let p = new Person { name: "Alice", age: 30 }

   # Won't work:
   # let p = Person { name: "Alice", age: 30 }
   ```

2. **Reference semantics for structs:**
   ```naab
   let alice = new Person { name: "Alice", age: 30 }
   let bob = alice  # bob and alice share same struct!
   bob.name = "Bob"
   print(alice.name)  # "Bob" (modified!)
   ```

3. **Semicolons optional everywhere:**
   ```naab
   let x = 42;    # OK
   let y = 43     # Also OK
   ```

---

## 8. Recent Changes (Last 8 Days)

### 2026-01-20 (TODAY) ✅

**Phase 2.4.6: Array Element Assignment - COMPLETE**
- ✅ List assignment: `arr[i] = value` with bounds checking
- ✅ Dict assignment: `dict[key] = value` (creates or updates)
- ✅ Sorting benchmark now works (bubble sort tested)
- ✅ All in-place algorithms unblocked
- **Effort:** 2 hours (estimated 2-3 days!)
- **Implementation:** 60 lines C++ in interpreter only

**Impact:**
- Can now implement: sorting, matrices, graphs, dynamic programming
- Benchmarking: 2/2 macro-benchmarks working (was 1/2)
- Critical features remaining: 3 (was 4)

### 2026-01-19 ✅

**Phase 3.2: Complete Tracing GC**
- ✅ Mark-and-sweep garbage collector
- ✅ Automatic GC triggering (threshold: 1000 allocations)
- ✅ Global value tracking with weak_ptr
- ✅ Out-of-scope cycle collection
- **Implementation:** 393 lines C++

**Phase 3.1: Enhanced Error Messages**
- ✅ Source location tracking (shows line:column)
- ✅ "Did you mean?" suggestions
- ✅ Color-coded error output
- **Implementation:** 20 lines C++

**Phase 3.3: Benchmarking Suite**
- ✅ 4 micro-benchmarks (variables, arithmetic, functions, strings)
- ✅ 2 macro-benchmarks (fibonacci, sorting)
- ✅ Runner scripts and documentation
- **Discovered:** 4 missing language features

### 2026-01-18 ✅

**Phase 2.4.4: Type Inference - COMPLETE**
- ✅ Variable inference: `let x = 42` → int
- ✅ Function return inference
- ✅ Generic argument inference
- **Implementation:** 361 lines C++

**Phase 3.1: Exception System Verification**
- ✅ 10/10 tests passing
- ✅ Try/catch/throw fully functional
- ✅ Stack traces working

### 2026-01-17 ✅

**Phase 2.4.1-2.4.5: Type System - COMPLETE**
- ✅ Generics with monomorphization
- ✅ Union types (`int | string`)
- ✅ Null safety (`int?` syntax)
- ✅ Enums

**Phase 5: Standard Library - COMPLETE**
- ✅ 13 modules implemented (4000+ lines C++)
- ✅ Native performance (10-100x faster)
- ✅ All modules tested and working

---

## 9. Status Documents Reference

### Where to Find Current State

1. **MASTER_STATUS.md** - Overall project status
   - Path: `~/.naab/language/MASTER_STATUS.md`
   - Updated: 2026-01-20
   - Progress: 79% complete

2. **PRODUCTION_READINESS_PLAN.md** - Implementation plan
   - Path: `~/.naab/language/PRODUCTION_READINESS_PLAN.md`
   - All phases with detailed checklists

3. **BENCHMARKING_SUMMARY.md** - Performance results
   - Path: `~/.naab/language/BENCHMARKING_SUMMARY.md`
   - 4/4 micro-benchmarks, 2/2 macro-benchmarks

4. **Session Documentation** - Daily work logs
   - Path: `~/.naab/language/docs/sessions/`
   - Latest: `PHASE_2_4_6_ARRAY_ASSIGNMENT_2026_01_20.md`

### Quick Status Check

```bash
cd ~/.naab/language

# View current status
cat MASTER_STATUS.md | head -20

# View recent changes
ls -lt docs/sessions/ | head -10

# Check benchmark results
cat BENCHMARKING_SUMMARY.md
```

---

## 10. Quick Command Reference

### Development

```bash
# Navigate to project
cd ~/.naab/language

# Build
cd build && make naab-lang

# Run test
./naab-lang run ../examples/test_*.naab

# Run benchmark
./naab-lang run ../benchmarks/micro/01_variables_simple.naab

# List all test files
ls ../examples/test_*.naab

# List all benchmarks
ls ../benchmarks/**/*.naab
```

### Testing

```bash
# Type system tests
./naab-lang run ../examples/test_phase2_4_1_generics.naab
./naab-lang run ../examples/test_phase2_4_2_union_types.naab
./naab-lang run ../examples/test_simple_inference.naab
./naab-lang run ../examples/test_nullable_simple.naab

# Runtime tests
./naab-lang run ../examples/test_phase3_1_exceptions_final.naab
./naab-lang run ../examples/test_gc_automatic_intensive.naab

# Array assignment tests (NEW!)
./naab-lang run ../test_array_assignment.naab
./naab-lang run ../test_array_assignment_complete.naab

# Benchmarks
./naab-lang run ../benchmarks/macro/fibonacci.naab
./naab-lang run ../benchmarks/macro/sorting.naab
```

### Documentation

```bash
# View this guide
cat ~/.naab/docs/AI_ASSISTANT_GUIDE.md

# View master status
cat ~/.naab/language/MASTER_STATUS.md

# View latest session
ls -lt ~/.naab/language/docs/sessions/ | head -5
```

---

## 11. Helping Users Write NAAb Code

### Step-by-Step Approach

1. **Understand the Task**
   - What are they trying to do?
   - What languages do they want to use?
   - Any performance requirements?

2. **Check Feature Availability**
   - ✅ Type system features? ALL AVAILABLE
   - ✅ Error handling? AVAILABLE
   - ✅ Array assignment? AVAILABLE (NEW!)
   - ❌ Range operator? NOT AVAILABLE (use while)
   - ❌ List methods? NOT AVAILABLE (use array module)

3. **Suggest Approach**
   ```naab
   # Native NAAb (best performance)
   use array
   use string
   fn process(data: list) -> list {
       return array.map(data, fn(x) { return x * 2 })
   }

   # Polyglot (when needed)
   fn analyze(data: list) -> dict {
       return <<python[data]
       import pandas as pd
       df = pd.DataFrame(data)
       return df.describe().to_dict()
       >>
   }
   ```

4. **Write Code Example**
   - Use type inference where possible
   - Handle errors with try/catch if needed
   - Use stdlib modules for common tasks
   - Show complete working example

5. **Explain Trade-offs**
   - Native NAAb: 10-100x faster
   - Polyglot: Slower but access to ecosystem

### Common Patterns

**File Processing:**
```naab
use io
use string

main {
    # Read file
    let content = io.read("data.txt")

    # Process lines
    let lines = string.split(content, "\n")

    # Process each line
    for line in lines {
        print(line)
    }
}
```

**HTTP API Call:**
```naab
use http
use json

main {
    # Fetch data
    let response = http.get("https://api.example.com/data")

    # Parse JSON
    let data = json.parse(response["body"])

    # Process
    print(data["name"])
}
```

**Data Transformation:**
```naab
use array

main {
    let nums = [1, 2, 3, 4, 5]

    # Double all numbers
    let doubled = array.map(nums, fn(x) { return x * 2 })

    # Filter evens
    let evens = array.filter(nums, fn(x) { return x % 2 == 0 })

    # Sum all
    let sum = array.reduce(nums, fn(acc, x) { return acc + x }, 0)
}
```

**Sorting (NEW!):**
```naab
use array

fn bubble_sort(arr: list) -> list {
    let n = array.length(arr)
    let i = 0
    while i < n {
        let j = 0
        while j < (n - i - 1) {
            if arr[j] > arr[j + 1] {
                # Swap using array assignment
                let temp = arr[j]
                arr[j] = arr[j + 1]
                arr[j + 1] = temp
            }
            j = j + 1
        }
        i = i + 1
    }
    return arr
}

main {
    let numbers = [5, 2, 8, 1, 9, 3]
    let sorted = bubble_sort(numbers)
    print(sorted)  # [1, 2, 3, 5, 8, 9]
}
```

---

## 12. Important Notes for AI Assistants

### DO ✅

1. **Check MASTER_STATUS.md first** - Always verify current state
2. **Use available features** - Type system 100% complete, use it!
3. **Suggest stdlib modules** - 10-100x faster than polyglot
4. **Show complete examples** - Users need working code
5. **Mention recent changes** - Array assignment just added!
6. **Handle errors properly** - Use try/catch/throw
7. **Use type inference** - Makes code cleaner
8. **Test your examples** - Verify syntax is correct

### DON'T ❌

1. **Don't suggest range operator** - Not implemented yet
2. **Don't use list methods** - Use `array.length()` not `list.length()`
3. **Don't forget bounds checking** - Array access can fail
4. **Don't ignore null safety** - Use `?` for nullable types
5. **Don't recommend outdated syntax** - Check docs first
6. **Don't assume features** - Verify in MASTER_STATUS.md
7. **Don't skip error handling** - Production code needs it
8. **Don't ignore performance** - Stdlib is much faster

### When User Reports a Bug

1. **Verify it's not a known limitation** - Check section 7
2. **Check recent changes** - Might be just fixed
3. **Ask for minimal reproduction** - Small test case
4. **Check MASTER_STATUS.md** - Feature might not be implemented
5. **Suggest workarounds** - If feature missing

---

## 13. Example Session

**User:** "I need to sort a list of numbers"

**AI:** "Great! As of 2026-01-20, array element assignment is now available, so sorting works! Here's a bubble sort implementation:

```naab
use array

fn bubble_sort(arr: list) -> list {
    let n = array.length(arr)
    let i = 0
    while i < n {
        let j = 0
        while j < (n - i - 1) {
            if arr[j] > arr[j + 1] {
                # Swap elements
                let temp = arr[j]
                arr[j] = arr[j + 1]
                arr[j + 1] = temp
            }
            j = j + 1
        }
        i = i + 1
    }
    return arr
}

main {
    let numbers = [5, 2, 8, 1, 9, 3]
    let sorted = bubble_sort(numbers)
    print(sorted)  # [1, 2, 3, 5, 8, 9]
}
```

**Note:** This feature was just added! Array element assignment (`arr[i] = value`) now works with bounds checking for safety. You can also use the built-in `array.sort()` for simpler cases."

---

## 14. Performance Guidelines

### Native vs Polyglot

**Native NAAb (stdlib):**
- ✅ 10-100x faster
- ✅ Type-safe
- ✅ Better error messages
- ❌ Limited ecosystem

**Polyglot:**
- ✅ Full Python/JS/etc ecosystem
- ✅ Easy integration with libraries
- ❌ 10-100x slower
- ❌ Type conversion overhead

### Optimization Tips

1. **Use stdlib when possible:**
   ```naab
   # Fast - native C++
   use array
   let sorted = array.sort(numbers)

   # Slow - polyglot overhead
   let sorted = <<python[numbers]
   sorted(numbers)
   >>
   ```

2. **Minimize polyglot calls:**
   ```naab
   # Bad - multiple calls
   for item in items {
       let result = <<python[item]
       process(item)
       >>
   }

   # Good - single call
   let results = <<python[items]
   [process(item) for item in items]
   >>
   ```

3. **Use type inference:**
   ```naab
   # Compiler optimizes better
   let x = 42              # Type known at compile time

   # vs
   let x: int | string = 42  # Runtime type checking
   ```

4. **Preallocate when possible:**
   ```naab
   # Build list incrementally (creates new list each time)
   let result = []
   for i in data {
       result = array.push(result, process(i))
   }

   # Better - use array assignment (in-place)
   let result = [0, 0, 0, 0, 0]  # Preallocate
   let i = 0
   for item in data {
       result[i] = process(item)
       i = i + 1
   }
   ```

---

## 15. Next Steps for Users

### Getting Started

1. **Try examples:**
   ```bash
   cd ~/.naab/language/build
   ./naab-lang run ../examples/test_simple_inference.naab
   ./naab-lang run ../test_array_assignment.naab
   ```

2. **Write your first program:**
   ```naab
   # hello.naab
   main {
       print("Hello from NAAb!")
   }
   ```

3. **Try type system features:**
   ```naab
   # types.naab
   struct Box<T> {
       value: T
   }

   main {
       let int_box = new Box<int> { value: 42 }
       let str_box = new Box<string> { value: "hello" }

       print(int_box.value)  # 42
       print(str_box.value)  # "hello"
   }
   ```

4. **Explore stdlib:**
   ```naab
   # stdlib.naab
   use array
   use string
   use math

   main {
       let nums = [1, 2, 3, 4, 5]
       let doubled = array.map(nums, fn(x) { return x * 2 })
       print(doubled)  # [2, 4, 6, 8, 10]
   }
   ```

### Learning Path

**Week 1:** Basic syntax, types, control flow
**Week 2:** Functions, structs, error handling
**Week 3:** Standard library, polyglot features
**Week 4:** Advanced types (generics, unions, null safety)
**Week 5:** Build real project!

---

## 16. Quick Troubleshooting

### Build Issues

**Problem:** CMake errors
```bash
# Solution: Clean and rebuild
cd ~/.naab/language
rm -rf build
mkdir build && cd build
cmake .. && make
```

**Problem:** Linker errors
```bash
# Solution: Check all stdlib modules built
ls -lh libnaab_stdlib.a
# Should see ~1-2MB library file
```

### Runtime Issues

**Problem:** "Command not found"
```bash
# Solution: Use full path or add to PATH
cd ~/.naab/language/build
./naab-lang run ../examples/test.naab
```

**Problem:** "Module not found"
```naab
# Solution: Use correct module name
use array as arr  # Correct
# use list as arr  # Wrong - no 'list' module
```

**Problem:** Seg fault or crash
```bash
# Check recent changes - might be known issue
cat MASTER_STATUS.md | grep -A 10 "Known"

# Try with debug info
gdb --args ./naab-lang run ../examples/test.naab
```

---

## 17. Version Information

**NAAb Version:** 0.1.0-alpha (Pre-release)
**Last Updated:** 2026-01-20
**Status:** 79% Production Ready

**Completed Phases:**
- ✅ Phase 1: Parser (100%)
- ✅ Phase 2: Type System (100%)
- ⚠️ Phase 3: Runtime (67%)
- ✅ Phase 5: Standard Library (100%)

**In Progress:**
- ⚠️ Phase 3.3: Performance optimization
- ❌ Phase 4: Tooling (design only)

**Estimated Production Ready:** ~2-3 months

---

## 18. Summary Checklist for AI Assistants

Before helping a user, verify:

- [ ] Read MASTER_STATUS.md for current state
- [ ] Check if feature is implemented (see section 7)
- [ ] Verify syntax is correct (see section 1)
- [ ] Use stdlib modules when possible (section 2)
- [ ] Handle errors with try/catch (section 1)
- [ ] Test examples if possible
- [ ] Mention recent changes if relevant (section 8)
- [ ] Suggest workarounds for missing features
- [ ] Check file paths are correct (section 3)
- [ ] Recommend performance best practices (section 14)

---

## 19. Emergency Contacts

**Documentation:**
- This guide: `~/.naab/docs/AI_ASSISTANT_GUIDE.md`
- Status: `~/.naab/language/MASTER_STATUS.md`
- Plan: `~/.naab/language/PRODUCTION_READINESS_PLAN.md`

**Session Logs:**
- `~/.naab/language/docs/sessions/` - Daily development logs

**Latest Changes:**
- `PHASE_2_4_6_ARRAY_ASSIGNMENT_2026_01_20.md` (TODAY!)
- `PHASE_3_3_BENCHMARKING_SUITE_2026_01_19.md`
- `COMPLETE_TRACING_GC_2026_01_19.md`

---

## End of Guide

**Remember:** NAAb is under active development. Always check MASTER_STATUS.md for the latest state. Array element assignment was just added today (2026-01-20), so documentation might lag behind implementation.

**When in doubt:**
1. Check MASTER_STATUS.md
2. Look at examples/ directory
3. Read latest session docs in docs/sessions/
4. Test with small example first

**Good luck helping users build with NAAb! 🚀**
