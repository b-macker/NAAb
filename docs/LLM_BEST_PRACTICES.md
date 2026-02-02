# NAAb Best Practices for LLMs

Guide for Large Language Models generating NAAb code. Avoid common pitfalls and produce idiomatic NAAb.

## Critical Rules

### 1. Entry Point: `main {}` NOT `fn main()`

❌ **WRONG:**
```naab
fn main() {
    io.write("Hello")
}
```

✅ **CORRECT:**
```naab
use io

main {
    io.write("Hello")
}
```

**Why:** NAAb uses a special `main {}` block, not a function.

### 2. Dictionary Keys MUST Be Quoted

❌ **WRONG:**
```naab
let person = {
    name: "Alice",
    age: 30
}
```

✅ **CORRECT:**
```naab
let person = {
    "name": "Alice",
    "age": 30
}
```

**Why:** NAAb dictionaries require quoted string keys (unlike JavaScript objects).

**Alternative:** Use structs for fixed schemas:
```naab
struct Person {
    name: string,
    age: int
}

let person = Person {
    name: "Alice",
    age: 30
}
```

### 3. Import Syntax: `use` NOT `import`

❌ **WRONG:**
```naab
import io from "std"
import { readFile } from "fs"
```

✅ **CORRECT:**
```naab
use io
use fs
```

**Why:** NAAb uses `use` for imports, not JavaScript's `import`.

### 4. Variable Declaration: `let` NOT `const` or `var`

❌ **WRONG:**
```naab
const MAX = 100
var count = 0
```

✅ **CORRECT:**
```naab
let MAX = 100
let count = 0
```

**Why:** NAAb only has `let` for variable declarations.

### 5. Dictionary Access: Brackets NOT Dots

❌ **WRONG:**
```naab
let name = person.name  // Only works on structs
```

✅ **CORRECT:**
```naab
let name = person["name"]  // For dictionaries
```

**Or use structs:**
```naab
struct Person { name: string }
let person = Person { name: "Alice" }
let name = person.name  // ✅ Dot notation works on structs
```

## Type System

### Type Inference

NAAb has strong type inference. Avoid unnecessary type annotations:

❌ **OVER-ANNOTATED:**
```naab
let x: int = 42  // Type obvious from initializer
let name: string = "Alice"
let items: list<int> = [1, 2, 3]
```

✅ **IDIOMATIC:**
```naab
let x = 42
let name = "Alice"
let items = [1, 2, 3]
```

**When to annotate:**
- Function parameters: `fn process(data: list<int>)`
- Return types: `fn calculate() -> int`
- When type isn't obvious: `let config: dict<string, any> = loadConfig()`

### Null Safety

NAAb has null values. Check before access:

```naab
fn safeDivide(a: int, b: int) -> int? {
    if b == 0 {
        return null
    }
    return a / b
}

fn useResult() {
    let result = safeDivide(10, 2)
    if result != null {
        io.write(result)
    }
}
```

## Functions

### Function Syntax

```naab
fn functionName(param1: type1, param2: type2) -> returnType {
    // body
    return value
}
```

### Avoid These Mistakes

❌ **WRONG - Python syntax:**
```naab
def calculate(x, y):
    return x + y
```

❌ **WRONG - Arrow function:**
```naab
const calculate = (x, y) => x + y
```

✅ **CORRECT:**
```naab
fn calculate(x: int, y: int) -> int {
    return x + y
}
```

## Control Flow

### If Statements

```naab
if condition {
    // then branch
} else if other {
    // else if branch
} else {
    // else branch
}
```

**Single-line:**
```naab
if x > 0 { return x }
```

### Loops

**For loop:**
```naab
for item in items {
    process(item)
}

for i in 0..10 {
    io.write(i)
}
```

**While loop:**
```naab
while condition {
    // body
}
```

**Don't use:**
- C-style for loops: `for (int i = 0; i < 10; i++)`  // ❌
- Python-style iteration: `for item in items:`  // ❌

## Data Structures

### Structs (Fixed Schema)

Use structs when you know the fields ahead of time:

```naab
struct Person {
    name: string,
    age: int,
    email: string
}

fn createPerson(name: string) -> Person {
    return Person {
        name: name,
        age: 0,
        email: ""
    }
}

let alice = createPerson("Alice")
let name = alice.name  // ✅ Dot notation
```

### Dictionaries (Dynamic Data)

Use dictionaries for dynamic key-value data:

```naab
fn buildConfig() -> dict<string, any> {
    return {
        "host": "localhost",
        "port": 8080,
        "debug": true
    }
}

let config = buildConfig()
let host = config["host"]  // ✅ Bracket notation
```

### Lists

```naab
let numbers = [1, 2, 3, 4, 5]
let names = ["Alice", "Bob", "Charlie"]
let mixed: list<any> = [1, "two", 3.0]
```

## Error Handling

### Try-Catch

```naab
fn riskyOperation() {
    try {
        dangerousCode()
    } catch (error) {
        io.write("Error: " + error)
    } finally {
        cleanup()
    }
}
```

### Throwing Errors

```naab
fn validate(x: int) {
    if x < 0 {
        throw "Value must be non-negative"
    }
}
```

## Polyglot Blocks

### Correct Syntax

✅ **WITH variable list:**
```naab
let data = [1, 2, 3, 4, 5]

let mean = <<python[data]  // Pass variables in brackets
import numpy as np
np.mean(data)
>>
```

❌ **WITHOUT variable list (WRONG):**
```naab
let mean = <<python
import numpy as np
np.mean(data)  // ❌ 'data' not available!
>>
```

### Supported Languages

- `python` - Python 3
- `javascript` - Node.js
- `rust` - Rust
- `cpp` - C++
- `shell` - Bash
- `go` - Go

### Async Operations

NAAb doesn't have built-in async/await yet. Use polyglot:

❌ **WRONG:**
```naab
async fn fetchData(url: string) -> dict<string, any> {
    let response = await http.get(url)  // ❌ Not implemented
    return response.json()
}
```

✅ **CORRECT:**
```naab
fn fetchData(url: string) -> dict<string, any> {
    return <<python[url]
import requests
response = requests.get(url)
response.json()
    >>
}
```

## Common LLM Mistakes

### 1. Over-Engineering

❌ **TOO COMPLEX:**
```naab
// Unnecessary abstraction
fn addWithLogging(a: int, b: int) -> int {
    logOperation("add", a, b)
    let result = performAddition(a, b)
    logResult(result)
    return result
}

fn performAddition(a: int, b: int) -> int {
    return a + b
}
```

✅ **SIMPLE:**
```naab
fn add(a: int, b: int) -> int {
    return a + b
}
```

### 2. Unnecessary Type Annotations

❌ **VERBOSE:**
```naab
let x: int = 42
let name: string = "Alice"
let items: list<int> = [1, 2, 3]
```

✅ **CLEAN:**
```naab
let x = 42
let name = "Alice"
let items = [1, 2, 3]
```

### 3. Wrong Error Handling Pattern

❌ **EMPTY CATCH:**
```naab
try {
    riskyCode()
} catch (error) {
    // Empty - swallows error
}
```

✅ **PROPER HANDLING:**
```naab
try {
    riskyCode()
} catch (error) {
    io.write("Error: " + error)
    // Or re-throw: throw error
}
```

### 4. JavaScript/Python Idioms

❌ **WRONG:**
```naab
// Python
if not condition:
    pass

// JavaScript
const result = items.map(x => x * 2)
const filtered = items.filter(x => x > 0)
```

✅ **NAAB:**
```naab
if !condition {
    // NAAb uses 'not' or '!' for negation
}

// Use loops or polyglot blocks for complex transformations
let result = []
for x in items {
    array.push(result, x * 2)
}
```

### 5. Module System Confusion

❌ **WRONG:**
```naab
// Node.js style
const fs = require('fs')
const data = fs.readFileSync('file.txt')

// ES6 style
import { readFile } from 'fs'
```

✅ **NAAB:**
```naab
use io

let content = io.readFile("file.txt")
```

## Idiomatic Patterns

### Simple is Better

```naab
// ✅ Direct and clear
fn isEven(n: int) -> bool {
    return n % 2 == 0
}

// ❌ Over-complicated
fn isEven(n: int) -> bool {
    if n % 2 == 0 {
        return true
    } else {
        return false
    }
}
```

### Early Returns

```naab
// ✅ Early return reduces nesting
fn process(data: list<int>) {
    if data.length == 0 {
        return
    }

    if !isValid(data) {
        return
    }

    // Main logic here
}

// ❌ Nested conditions
fn process(data: list<int>) {
    if data.length > 0 {
        if isValid(data) {
            // Main logic deeply nested
        }
    }
}
```

### Descriptive Names

```naab
// ✅ Clear intent
fn calculateMonthlyPayment(principal: int, rate: float, years: int) -> float {
    // ...
}

// ❌ Cryptic
fn calc(p: int, r: float, y: int) -> float {
    // ...
}
```

## Testing Generated Code

Before providing NAAb code, mentally check:

1. ✅ Uses `main {}` not `fn main()`
2. ✅ Dictionary keys are quoted
3. ✅ Uses `use` not `import`
4. ✅ Uses `let` not `const` or `var`
5. ✅ Type annotations only where needed
6. ✅ Bracket notation for dict access
7. ✅ Polyglot blocks have variable lists
8. ✅ No JavaScript/Python idioms

## Resources

- [Language Reference](../README.md)
- [Error Messages](ERROR_MESSAGES.md)
- [Debugging Guide](DEBUGGING_GUIDE.md)
- [Examples](../examples/)

## Quick Reference Card

```
Entry Point:        main { }
Variables:          let x = 42
Functions:          fn name(x: type) -> type { }
Import:             use module
Structs:            struct Name { field: type }
Struct Literal:     Name { field: value }
Dict Literal:       {"key": value}
List Literal:       [1, 2, 3]
Dict Access:        dict["key"]
Struct Access:      struct.field
For Loop:           for item in list { }
If Statement:       if cond { } else { }
Try-Catch:          try { } catch (e) { }
Polyglot:           <<lang[vars] code >>
Comments:           // single line
                    /* multi line */
```

## Remember

When in doubt, generate simple, straightforward NAAb code. Avoid carrying over patterns from other languages. Let NAAb's type inference work for you. Keep it clean and idiomatic! 🚀
