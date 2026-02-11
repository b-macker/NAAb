# NAAb Variable Binding in Inline Polyglot Code

## Overview

When using inline polyglot code in NAAb, you must **explicitly declare** which NAAb variables should be accessible to the embedded code using the variable binding syntax.

## Syntax

```naab
let result = <<language[var1, var2, ...]
embedded code using var1, var2
>>
```

The `[var1, var2, ...]` section explicitly lists which NAAb variables to bind into the polyglot context.

## ❌ Common Mistake: Forgetting Variable Binding

```naab
// This FAILS with "NameError: name 'y' is not defined"
let y = 10
let result = <<python
y * 2
>>
```

**Error Message:**
```
Error: Parallel polyglot execution failed in block 0:
  Python error: NameError: name 'y' is not defined

  Help: Did you forget to bind a NAAb variable?
  Inline polyglot code requires explicit variable binding syntax.

  ✗ Wrong - variable not bound:
    let result = <<python
    y * 2
    >>

  ✓ Right - explicit variable binding:
    let result = <<python[y]
    y * 2
    >>
```

## ✅ Correct Usage

### Single Variable

```naab
let y = 10
let result = <<python[y]
y * 2
>>
print(result)  // 20
```

### Multiple Variables

```naab
let a = 10
let b = 20
let sum = <<python[a, b]
a + b
>>
print(sum)  // 30
```

### All Polyglot Languages

This applies to **all** polyglot languages:

**Python:**
```naab
let x = 5
let result = <<python[x]
x ** 2
>>
```

**JavaScript:**
```naab
let x = 5
let result = <<javascript[x]
x * x
>>
```

**Rust:**
```naab
let x = 5
let result = <<rust[x]
x * x
>>
```

**C++:**
```naab
let x = 5
let result = <<cpp[x]
x * x
>>
```

**Shell:**
```naab
let filename = "test.txt"
let result = <<bash[filename]
cat $filename
>>
```

## How It Works

1. **Parser Detection:** The parser looks for `[var1, var2]` syntax after the language name
2. **Variable Capture:** During execution, NAAb captures the current values of listed variables
3. **Code Injection:** Variables are serialized and injected as declarations in the target language:
   - Python: `y = 10\n`
   - JavaScript: `const y = 10;\n`
   - Rust: `let y = 10;\n`
   - C++: `const auto y = 10;\n`
   - Shell: `export y=10\n`
4. **Execution:** The polyglot code runs with access to these variables

## Type Conversion

NAAb automatically converts variable types to the target language:

```naab
let num = 42
let text = "hello"
let arr = [1, 2, 3]
let dict = {"key": "value"}

let result = <<python[num, text, arr, dict]
print(num)      # 42
print(text)     # "hello"
print(arr)      # [1, 2, 3]
print(dict)     # {"key": "value"}
>>
```

## No Automatic Detection

NAAb does **not** automatically detect which variables are used in the code. You must explicitly list them:

```naab
// ❌ This fails even though 'x' is obviously used
let x = 10
let result = <<python
x * 2  // NameError: name 'x' is not defined
>>

// ✅ Must explicitly bind 'x'
let x = 10
let result = <<python[x]
x * 2  // Works: 20
>>
```

## Debugging Tips

1. **Check the error message** - it will suggest the correct syntax
2. **List all variables** - include every NAAb variable your code uses
3. **Order doesn't matter** - `[a, b]` and `[b, a]` work the same
4. **Whitespace is flexible** - `[a,b]` and `[a, b]` are equivalent

## Summary

- ✅ Always use `<<language[var1, var2] code >>` when referencing NAAb variables
- ✅ List all variables you need, separated by commas
- ✅ Works for all polyglot languages (Python, JavaScript, Rust, C++, Shell, etc.)
- ❌ No automatic variable detection - you must be explicit
- ❌ Forgetting `[var]` causes NameError/ReferenceError with helpful guidance

## Test Examples

See these test files for working examples:
- `tests/bugs/test_inline_debug.naab` - Single and multiple variable binding
- `tests/bugs/test_variable_binding_error.naab` - Error message demonstration
