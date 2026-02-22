# Common Entry Point Mistakes

This document explains the most common mistake beginners make when starting with NAAb: using `fn main()` instead of `main {}`.

## The Correct Syntax

NAAb uses a special `main {}` block as the program entry point:

```naab
main {
    print("Hello, NAAb!")
}
```

This is **not a function definition**. It's a special top-level construct that serves as the program's entry point.

## Common Mistake #1: Using `fn main()`

Many developers coming from languages like Rust, C, or Go try to use:

```naab
fn main() {  // ❌ ERROR!
    print("Hello!")
}
```

**Error message:**
```
Parse error at line X, column Y: Expected function name after 'fn'
Got: 'main'
```

**Why this fails:** The `fn` keyword is used for defining **named functions**, not the entry point. When the parser sees `fn`, it expects a function name, but `main` is a reserved keyword for the entry point block.

## Common Mistake #2: Calling main as a function

```naab
main() {  // ❌ ERROR!
    print("Hello!")
}
```

**Error message:**
```
Parse error: Unexpected token
```

**Why this fails:** `main` is not a function call. It's a special keyword that introduces the entry point block.

## Common Mistake #3: Using void or return types

```naab
fn main() -> void {  // ❌ ERROR!
    print("Hello!")
}
```

**Why this fails:** Same as mistake #1. The entry point is not a function definition.

## The Right Way: Understanding `main {}`

Think of `main {}` as a special **top-level block**, similar to:
- Python's `if __name__ == "__main__":` block
- JavaScript's immediate script execution
- Shell script's top-level commands

```naab
// ✅ CORRECT - This is the entry point block
main {
    // Your code starts executing here
    print("Hello, World!")
}

// You CAN define functions separately
fn greet(name: string) {
    print("Hello, ", name, "!")
}

// ✅ Functions are defined with 'fn'
// ✅ Entry point is 'main {}'
```

## Why This Design?

This design makes NAAb code more readable:
1. **Clear separation**: Functions use `fn`, entry point uses `main {}`
2. **No confusion**: You can't accidentally define a function called `main`
3. **Simplicity**: No need for function signatures on the entry point

## Key Takeaways

| ✅ Correct | ❌ Wrong | Why Wrong |
|-----------|---------|-----------|
| `main { ... }` | `fn main() { ... }` | Entry point is not a function |
| `fn greet() { ... }` | `main greet() { ... }` | Functions use `fn`, not `main` |
| Functions defined separately | Passing args to `main` | Entry point takes no parameters |

## See Also

- Chapter 1.4.1: Understanding the Entry Point
- Chapter 1.6: Common Beginner Mistakes
- `hello.naab` - Working example with anti-patterns documented
