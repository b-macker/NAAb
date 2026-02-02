# Common Dictionary Syntax Mistakes

This document explains the most common mistakes when working with dictionaries in NAAb, particularly the difference between dictionary and struct syntax.

## The Correct Dictionary Syntax

NAAb dictionaries require **quoted string keys** and **bracket notation** for access:

```naab
let person = {
    "name": "Alice",
    "age": 30,
    "city": "New York"
}

let name = person["name"]  // ✅ Bracket notation
print(name)  // Output: Alice
```

## Common Mistake #1: Unquoted Dictionary Keys

Many developers try to use JavaScript/JSON5-style unquoted keys:

```naab
let person = {
    name: "Alice",    // ❌ ERROR!
    age: 30,          // ❌ ERROR!
    city: "New York"  // ❌ ERROR!
}
```

**Error message:**
```
Parse error: Expected '}' or ','
Got: ':'
```

**Why this fails:** NAAb's parser requires dictionary keys to be quoted strings. This ensures clear distinction between dictionaries (runtime dynamic) and structs (compile-time typed).

**Fix:**
```naab
let person = {
    "name": "Alice",    // ✅ Quoted keys
    "age": 30,
    "city": "New York"
}
```

## Common Mistake #2: Dot Notation on Dictionaries

Developers familiar with JavaScript often try to use dot notation:

```naab
let person = {"name": "Alice", "age": 30}
let name = person.name  // ❌ ERROR!
```

**Error message:**
```
Error: Member access not supported on this type
```

**Why this fails:** Dot notation (`.`) is reserved for **struct field access**, not dictionary key access. Dictionaries use bracket notation.

**Fix:**
```naab
let person = {"name": "Alice", "age": 30}
let name = person["name"]  // ✅ Bracket notation
```

## Common Mistake #3: Using Reserved Keywords as Keys

NAAb has reserved keywords that cannot be used even as quoted dictionary keys in some contexts:

```naab
let config = {
    "type": "production",  // ✅ Usually OK
    "config": "settings"   // ⚠️ May cause issues (config is reserved)
}
```

**Fix:** Use different key names:
```naab
let settings = {
    "type": "production",
    "configuration": "settings"  // ✅ Avoid reserved words
}
```

## Common Mistake #4: Confusing Structs and Dictionaries

**Structs** and **Dictionaries** look similar but work differently:

```naab
// ❌ Mixing struct and dictionary syntax
struct Person {
    name: string,
    age: int
}

let alice = Person {
    "name": "Alice",  // ❌ ERROR: Struct fields don't use quotes
    "age": 30
}
```

**Fix - Know the difference:**

```naab
// ✅ Struct: Compile-time typed, unquoted fields, dot access
struct Person {
    name: string,
    age: int
}

let alice = Person {
    name: "Alice",    // Unquoted in struct literal
    age: 30
}
let name = alice.name  // Dot notation for structs

// ✅ Dictionary: Runtime dynamic, quoted keys, bracket access
let bob = {
    "name": "Bob",    // Quoted in dictionary
    "age": 25
}
let name2 = bob["name"]  // Bracket notation for dicts
```

## Comparison Table

| Feature | Struct | Dictionary |
|---------|--------|------------|
| **Key syntax in literal** | Unquoted: `name: value` | Quoted: `"name": value` |
| **Access syntax** | Dot notation: `obj.field` | Bracket notation: `dict["key"]` |
| **Type safety** | Compile-time checked | Runtime dynamic |
| **Key flexibility** | Fixed at definition | Can add/remove at runtime |
| **Use case** | Known structure | Dynamic/JSON-like data |

## When to Use Each

**Use Structs when:**
- You know the structure at compile time
- You want type safety
- You need better performance
- Fields are accessed frequently

```naab
struct Config {
    host: string,
    port: int,
    debug: bool
}

let cfg = Config {
    host: "localhost",
    port: 8080,
    debug: true
}
```

**Use Dictionaries when:**
- Structure is dynamic/unknown
- Working with JSON data
- Need runtime flexibility
- Implementing maps/key-value stores

```naab
let json_data = {
    "userId": "12345",
    "settings": {
        "theme": "dark",
        "notifications": true
    }
}
```

## Key Takeaways

| ✅ Correct | ❌ Wrong | Reason |
|-----------|---------|--------|
| `{"name": "Alice"}` | `{name: "Alice"}` | Dictionary keys must be quoted |
| `dict["key"]` | `dict.key` | Dictionaries use bracket notation |
| `struct.field` | `struct["field"]` | Structs use dot notation |
| `"configuration"` | `"config"` | Avoid reserved keywords |

## Common Patterns

### Working with JSON

```naab
use json

main {
    let data = {"name": "Alice", "age": 30}
    let json_str = json.stringify(data)
    print(json_str)  // {"name":"Alice","age":30}

    let parsed = json.parse(json_str)
    print(parsed["name"])  // Alice
}
```

### Dynamic Key Access

```naab
let user = {"name": "Bob", "email": "bob@example.com"}

// Dynamic key lookup
let key = "email"
let value = user[key]  // Works with variables too!
print(value)  // bob@example.com
```

## See Also

- Chapter 2.3.2: Dictionaries: Syntax and Access Patterns
- Chapter 2.4.3: Structs vs. Dictionaries comparison
- `dictionaries.naab` - Correct dictionary syntax examples
- `structs_vs_dicts.naab` - Side-by-side comparison
