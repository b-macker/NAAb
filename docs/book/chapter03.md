# Chapter 3: Control Flow

Control flow statements are the backbone of any programming language, allowing you to execute different blocks of code based on conditions or to repeat actions multiple times. This chapter explores NAAb's constructs for managing program flow.

## 3.1 Conditional Execution: `if`, `else if`, and `else`

NAAb uses `if`, `else if`, and `else` statements to execute code conditionally. The condition inside an `if` or `else if` statement must evaluate to a boolean (`true` or `false`). Unlike some languages, parentheses around the condition are optional.

```naab
main {
    let temperature = 25

    // Basic if statement
    if temperature > 20 {
        print("It's a warm day!")
    }

    // if-else statement
    if temperature < 10 {
        print("It's cold!")
    } else {
        print("It's not too cold.")
    }

    // if-else if-else chain
    let score = 85
    if score >= 90 {
        print("Grade: A")
    } else if score >= 80 {
        print("Grade: B")
    } else if score >= 70 {
        print("Grade: C")
    } else {
        print("Grade: F")
    }
}
```
**Output of the above code:**
```
It's a warm day!
It's not too cold.
Grade: B
```

## 3.2 Loops: `while` and `for..in`

NAAb provides `while` loops for repeating a block of code as long as a condition is `true`, and `for..in` loops for iterating over collections.

### 3.2.1 The `while` Loop

A `while` loop continues to execute its body as long as its condition remains `true`. It's crucial to ensure the condition eventually becomes `false` to avoid infinite loops.

```naab
main {
    let countdown = 3
    print("Starting countdown...")
    while countdown > 0 {
        print(" ", countdown, "...")
        countdown = countdown - 1 // Decrement to eventually end the loop
    }
    print("Liftoff!")
}
```
**Output of the above code:**
```
Starting countdown...
 3 ...
 2 ...
 1 ...
Liftoff!
```

### 3.2.2 The `for..in` Loop

The `for..in` loop is used to iterate over elements of collections like arrays. It assigns each element, in turn, to a specified variable for the duration of one loop iteration.

```naab
main {
    let fruits = ["apple", "banana", "cherry"]
    print("Available fruits:")
    for fruit in fruits {
        print(" - ", fruit)
    }

    let user_data = {"name": "Alice", "age": 30, "city": "New York"}
    print("User data details:")
    for key in user_data { // Iterating over keys of a dictionary
        print(" - ", key, ": ", user_data[key])
    }
}
```
**Output of the above code:**
```
Available fruits:
 -  apple
 -  banana
 -  cherry
User data details:
 -  name : Alice
 -  age : 30
 -  city : New York
```

### 3.2.3 Range Loops

NAAb supports numeric ranges for iterating a specific number of times.

*   **Exclusive Range (`..`)**: Iterates from start up to (but not including) end.
*   **Inclusive Range (`..=`)**: Iterates from start up to and including end.

```naab
main {
    print("Exclusive (1..5):")
    for i in 1..5 {
        print(i) // Prints 1, 2, 3, 4
    }

    print("Inclusive (1..=5):")
    for i in 1..=5 {
        print(i) // Prints 1, 2, 3, 4, 5
    }
}
```

## 3.3 Flow Control Keywords: `break` and `continue`

Inside loops, you can use `break` and `continue` statements to alter the normal flow of execution.

*   **`break`**: Immediately exits the innermost loop. Execution continues with the statement immediately following the loop.
*   **`continue`**: Skips the rest of the current loop iteration and proceeds to the next iteration (evaluating the loop's condition again for `while` loops, or fetching the next element for `for..in` loops).

```naab
main {
    print("Demonstrating break and continue:")
    let count = 0
    while count < 10 {
        count = count + 1

        if count == 3 {
            continue // Skip printing 3
        }

        if count == 7 {
            break // Exit loop when count reaches 7
        }

        print("Current count:", count)
    }
    print("Loop finished.")
}
```
**Output of the above code:**
```
Demonstrating break and continue:
Current count: 1
Current count: 2
Current count: 4
Current count: 5
Current count: 6
Loop finished.
```
Notice that `3` and `7` were not printed. `3` was skipped by `continue`, and the loop terminated before `7` could be printed due to `break`.

## 3.4 Best Practices for Control Flow

*   **Clarity over Cleverness**: Aim for clear and readable control flow. Overly complex nested `if` statements or intricate loop conditions can be hard to understand and maintain.
*   **Avoid Infinite Loops**: Always ensure `while` loop conditions eventually become false. Use `break` judiciously to prevent unintended infinite loops.
*   **Use `for..in` for Collections**: When iterating over arrays or processing dictionary keys, `for..in` is generally more idiomatic and safer than manual index-based `while` loops.
*   **Early Exit with `if`**: For functions, use `if` statements to handle edge cases or invalid inputs early, reducing nesting and improving readability.
