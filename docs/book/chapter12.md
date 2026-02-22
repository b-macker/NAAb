# Chapter 12: Error Handling

Robust error handling is critical for building reliable applications. NAAb provides a comprehensive exception handling system similar to languages like Java and JavaScript, featuring `try`, `catch`, and `finally` blocks, along with stack trace support.

## 12.1 The `try`, `catch`, `finally` Mechanism

To handle potential errors gracefully, wrap risky code in a `try` block. If an error occurs, execution jumps to the `catch` block. The `finally` block, if present, executes regardless of whether an error occurred.

```naab
main {
    print("Starting operation...")

    try {
        // Simulate a risky operation
        let x = 10
        let y = 0
        
        // This will throw a "Division by zero" error
        if y == 0 {
            throw "Division by zero error"
        }
        
        print("Result:", x / y)

    } catch (error) {
        print("Caught an error:", error)
        // You can inspect the error or perform recovery here

    } finally {
        print("Cleanup: Operation complete.")
    }
    
    print("Program continues...")
}
```

**Output:**
```
Starting operation...
Caught an error: Division by zero error
Cleanup: Operation complete.
Program continues...
```

## 12.2 Throwing Errors

You can generate your own errors using the `throw` keyword. You can throw strings, or potentially other types depending on your error handling strategy.

```naab
fn validate_age(age: int) {
    if age < 0 {
        throw "Invalid age: cannot be negative"
    }
    print("Age is valid:", age)
}

main {
    try {
        validate_age(-5)
    } catch (e) {
        print("Validation failed:", e)
    }
}
```

## 12.3 Error Propagation

If an exception is thrown inside a function and not caught, it propagates up the call stack to the caller. This continues until a matching `catch` block is found or the program terminates (printing the error and stack trace).

```naab
fn risky_function() {
    throw "Something went wrong!"
}

fn safe_wrapper() {
    print("Calling risky function...")
    risky_function()
    print("This line will not be reached.")
}

main {
    try {
        safe_wrapper()
    } catch (e) {
        print("Caught in main:", e)
    }
}
```

**Output:**
```
Calling risky function...
Caught in main: Something went wrong!
```

## 12.4 Debugging Strategies

When errors occur, NAAb provides stack traces to help you pinpoint the location. Additionally, NAAb includes an interactive debugger for step-through debugging and comprehensive error hints to help you understand and fix issues quickly.

### 12.4.1 Enhanced Error Messages

NAAb provides context-aware error messages with fix suggestions:

```
Error: Expected '}' or ',' but got ':' at line 3, column 9

Hint: Dictionary keys must be quoted strings in NAAb.

Did you mean:
    let person = {
        "name": "Alice",  // ✅ Quoted keys
        "age": 30
    }

Instead of:
    let person = {
        name: "Alice",  // ❌ Unquoted keys
        age: 30
    }

Note: Use structs for fixed schemas, dictionaries for dynamic data.
```

Error messages include:
*   **Source context**: Shows the problematic line with highlighting
*   **Helpful hints**: Explains what went wrong and why
*   **Fix examples**: Shows correct code alongside the error
*   **Similar symbols**: Suggests similar variable/function names if you have a typo

### 12.4.2 Interactive Debugger

For complex debugging scenarios, use the interactive debugger:

```bash
naab-lang run --debug myprogram.naab
```

**Setting Breakpoints:**

Add breakpoints directly in your code:

```naab
fn calculate_total(prices: list<int>) -> int {
    let total = 0
    for price in prices {
        // breakpoint
        total = total + price    // Debugger pauses here
    }
    return total
}
```

**Debugger Commands:**

When execution pauses, you can:
*   `c` or `continue` - Continue execution until next breakpoint
*   `s` or `step` - Step to next line (enters functions)
*   `n` or `next` - Step over function calls
*   `v` or `vars` - Show all local variables
*   `p <expr>` or `print <expr>` - Evaluate and print expression
*   `w <expr>` or `watch <expr>` - Add watch expression
*   `b <file>:<line>` - Set breakpoint at specific line
*   `h` or `help` - Show all commands
*   `q` or `quit` - Exit debugger

**Example Debug Session:**

```bash
$ naab-lang run --debug fibonacci.naab

Breakpoint hit at fibonacci.naab:14

(debug) vars

--- Local Variables ---
a = 0
b = 1
i = 2

(debug) p a + b
1

(debug) watch temp
Added watch: temp

(debug) continue
[Step 15] temp=1
[Step 16] temp=2
[Step 17] temp=3
```

### 12.4.3 Watch Expressions

Monitor how variables change during execution:

```bash
naab-lang run --debug --watch="total,count" myprogram.naab
```

The debugger will automatically print these values at each step:

```
[Step 10] total=42, count=5
[Step 11] total=47, count=6
```

### 12.4.4 Traditional Debugging Techniques

Even without the debugger, several techniques remain useful:

1.  **Read the Stack Trace**: Look for the filename and line number in the error message. NAAb's enhanced stack traces show:
    *   Function name and location
    *   Call hierarchy
    *   Local variable values at each frame

2.  **Use `print` Debugging**: Insert `print` statements to trace variable values and execution flow:
    ```naab
    fn processData(data: list<int>) {
        print("DEBUG: data =", data)
        print("DEBUG: length =", data.length)
        // ... rest of function
    }
    ```

3.  **Isolate Blocks**: If an error occurs in a polyglot block, try running that code snippet in its native environment (e.g., Python shell) to verify its logic.

4.  **Use Try-Catch for Exploration**: Wrap suspicious code in try-catch to see exactly what error is thrown:
    ```naab
    try {
        risky_operation()
    } catch (e) {
        print("Error details:", e)
        // Examine error to understand what went wrong
    }
    ```

### 12.4.5 Common Error Patterns

NAAb's enhanced error system detects common mistakes:

**Undefined Variable with Suggestions:**
```
RuntimeError: Undefined variable 'confg' at line 12, column 5

Hint: Variable 'confg' is not defined. Did you mean one of these?
    - config (defined at line 5)
    - cfg (imported from 'settings')
```

**Type Mismatch:**
```
RuntimeError: Type mismatch at line 15

Expected: int
Actual: string

Hint: Cannot add string to int. Consider:
    - Convert string to int: parseInt(value)
    - Convert int to string: toString(count)
```

**Null Access:**
```
RuntimeError: Cannot access property of null at line 20

Hint: Always check for null before accessing properties:
    if result != null {
        use(result.value)  // ✅ Safe
    }
```

For a complete guide to debugging NAAb programs, see [Chapter 16: Tooling](chapter16.md).
