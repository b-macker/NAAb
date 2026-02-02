# Chapter 2: Variables, Types, and Mutability

In NAAb, understanding how to declare variables, work with different data types, and manage mutability is fundamental. This chapter dives into the core aspects of data handling within the language.

## 2.1 Declaring Variables with `let`

Variables in NAAb are declared using the `let` keyword. Once declared, a variable can hold a value of any type, as NAAb supports type inference. You can also explicitly specify a type using a colon (`:`) followed by the type name.

```naab
main {
    // Type inferred: int
    let count = 10

    // Type inferred: string
    let message = "Hello, NAAb!"

    // Explicit type declaration
    let temperature: float = 25.5

    // Boolean value
    let is_active: bool = true

    print("Count:", count)          // Output: Count: 10
    print("Message:", message)      // Output: Message: Hello, NAAb!
    print("Temperature:", temperature) // Output: Temperature: 25.5
    print("Is Active:", is_active)     // Output: Is Active: true
}
```

Variables declared with `let` are mutable by default. You can reassign their values.

```naab
main {
    let score = 100
    print("Initial score:", score) // Output: Initial score: 100

    score = 150 // Reassigning the value
    print("New score:", score)     // Output: New score: 150
}
```
**Note on `let` Scope:** While `let` can technically declare global variables, it's generally recommended that `let` declarations appear within function bodies (e.g., inside `main {}`). When `use` statements or type definitions (structs, enums) are present globally, directly followed by a global `let` declaration, the parser may interpret this as an error. For robust code, declare variables within functions.

## 2.2 Primitive Data Types

NAAb supports a concise set of primitive data types:

*   **`int`**: For whole numbers (e.g., `42`, `-5`, `0`).
*   **`float`**: For decimal numbers (e.g., `3.14`, `0.0`, `-1.5`).
*   **`string`**: For sequences of characters, enclosed in double quotes (e.g., `"hello world"`).
*   **`bool`**: For boolean values, either `true` or `false`.

```naab
main {
    let int_val: int = 123
    let float_val: float = 45.67
    let string_val: string = "NAAb is awesome"
    let bool_val: bool = false

    print("Int:", int_val)
    print("Float:", float_val)
    print("String:", string_val)
    print("Bool:", bool_val)
}
```

## 2.3 Compound Data Types: Arrays and Dictionaries

Beyond primitives, NAAb provides built-in support for common compound data types: arrays and dictionaries.

### 2.3.1 Arrays

Arrays (often called lists in other languages) are ordered collections of values. They are declared using square brackets `[]`.

```naab
main {
    let numbers = [10, 20, 30, 40]             // Array of ints
    let names: array<string> = ["Alice", "Bob"] // Explicit type: array of strings
    let mixed_array = [1, "two", true]         // Dynamically typed array

    print("Numbers:", numbers)       // Output: Numbers: [10, 20, 30, 40]
    print("First name:", names[0])   // Output: First name: Alice

    // Accessing elements
    print("Third number:", numbers[2]) // Output: Third number: 30
}
```

A significant feature in NAAb is the ability to modify array elements in place using index assignment.

```naab
main {
    let colors = ["red", "green", "blue"]
    print("Original colors:", colors) // Output: Original colors: ["red", "green", "blue"]

    colors[1] = "yellow" // Modifying an element
    print("Modified colors:", colors) // Output: Modified colors: ["red", "yellow", "blue"]

    // Attempting to access an out-of-bounds index will result in a runtime error.
}
```

### 2.3.2 Dictionaries: Syntax and Access Patterns

Dictionaries (also known as maps or hash tables) are unordered collections of key-value pairs. Keys must be strings, and values can be of any type. They are declared using curly braces `{}`.

**IMPORTANT: Dictionary keys MUST be quoted strings, and values are accessed using bracket notation `[]`.**

✅ **Correct dictionary syntax:**
```naab
use io

main {
    let person = {
        "name": "Alice",      // ✅ Keys are quoted strings
        "age": 30,
        "is_student": true
    }
    let config: dict<string, any> = { // Explicitly typed dict
        "debug_mode": false,
        "api_key": "some_secret"
    }

    io.write("Person: ", person, "\n")
    io.write("Person's name: ", person["name"], "\n")  // ✅ Bracket notation

    // Modifying a value
    person["age"] = 31
    io.write("New age: ", person["age"], "\n")

    // Adding a new key-value pair
    person["city"] = "New York"
    io.write("Person with city: ", person, "\n")
}
```

❌ **Common mistakes:**
```naab
main {
    // MISTAKE 1: Unquoted keys (treated as variable references)
    let broken = {
        name: "Alice",    // ERROR: Undefined variable: name
        age: 30
    }

    let person = {"name": "Alice"}

    // MISTAKE 2: Using dot notation (only works for structs, not dicts)
    let name = person.name  // ERROR: Dot notation doesn't work on dicts
}
```

**Key Rules:**
1. **Keys are strings** - Always use quotes: `"key"`
2. **Access with brackets** - Use `dict["key"]` not `dict.key`
3. **Dynamic keys** - Dictionaries are for data with unknown or dynamic keys
4. **No type checking** - Dictionary values can be any type (checked at runtime)

## 2.4 User-Defined Types: Structs

NAAb allows you to define custom data structures called `structs`. Structs are blueprints for creating objects that group related data together. They provide type safety and improved code readability compared to untyped dictionaries.

### 2.4.1 Defining Structs

Structs are defined using the `struct` keyword, followed by the struct's name and a block containing its fields and their types.

```naab
// Define a Point struct outside of main
struct Point {
    x: int;
    y: int;
    label: string;
}

// Define a User struct
struct User {
    id: int;
    username: string;
    email: string;
    is_active: bool;
}

main {
    // ... struct usage ...
}
```

### 2.4.2 Instantiating Structs with `new`

To create an instance of a struct, use the `new` keyword followed by the struct's name and a block containing the field values.

```naab
// Define a Point struct
struct Point {
    x: int;
    y: int;
    label: string;
}

main {
    let origin = new Point {
        x: 0,
        y: 0,
        label: "Origin"
    }

    let target = new Point {
        x: 100,
        y: 200,
        label: "Target"
    }

    print("Origin:", origin.label, "at (", origin.x, ", ", origin.y, ")")
    print("Target X:", target.x)

    // Accessing and modifying fields
    origin.x = 5
    print("Modified Origin X:", origin.x) // Output: Modified Origin X: 5
}
```
**Important:** The `new` keyword is crucial for distinguishing struct instantiation from dictionary literals. Omitting `new` will cause the parser to treat `{...}` as a dictionary.

### 2.4.3 Structs vs. Dictionaries: When to Use Which?

A common question for new NAAb programmers is: "When should I use a struct, and when should I use a dictionary?" Understanding the differences will help you write cleaner, more maintainable code.

#### Quick Comparison Table

| Feature | Struct | Dictionary |
|---------|--------|------------|
| **Keys/Fields** | Identifiers (unquoted) | Strings (quoted) |
| **Access syntax** | `obj.field` (dot notation) | `dict["key"]` (bracket notation) |
| **Type checking** | Compile-time | Runtime |
| **Field definition** | Fixed at definition | Dynamic at runtime |
| **When to use** | Known structure | Dynamic/JSON data |
| **Performance** | Slightly faster | Slightly slower |
| **Type safety** | Strong | Weak |

#### Side-by-Side Example

```naab
use io

// Define a struct with known fields
struct Person {
    name: string
    age: int
    email: string
}

main {
    // STRUCTS: Known structure, type-safe
    let alice = new Person {
        name: "Alice",     // Unquoted field names
        age: 30,
        email: "alice@example.com"
    }

    // Access with dot notation
    io.write("Name: ", alice.name, "\n")
    io.write("Age: ", alice.age, "\n")

    // Field assignment
    alice.age = 31
    // alice.age = "thirty"  // ERROR: Type mismatch (compile-time)

    // DICTIONARIES: Dynamic structure, flexible
    let bob = {
        "name": "Bob",     // Quoted key names
        "age": 25,
        "role": "developer"
    }

    // Access with bracket notation
    io.write("Name: ", bob["name"], "\n")
    io.write("Age: ", bob["age"], "\n")

    // Can add new keys dynamically
    bob["department"] = "Engineering"

    // No type checking at assignment
    bob["age"] = "twenty-five"  // OK at compile-time, but might cause issues later
}
```

#### When to Use Structs

Use structs when:
- ✅ You know all the fields ahead of time
- ✅ You want compile-time type checking
- ✅ The data structure represents a concrete entity (User, Point, Config)
- ✅ You want IDE autocomplete and refactoring support
- ✅ Performance matters (structs are slightly faster)

**Example use cases:**
- Configuration objects
- Data models (User, Product, Order)
- Mathematical structures (Point, Vector, Matrix)
- Application state

#### When to Use Dictionaries

Use dictionaries when:
- ✅ The keys are not known at compile time
- ✅ You're parsing JSON or other dynamic data
- ✅ You need to add/remove keys at runtime
- ✅ The structure varies between instances
- ✅ You're building generic data processors

**Example use cases:**
- JSON API responses
- User-provided configuration
- Key-value caches
- Dynamic attribute storage
- Parsing unknown data formats

#### Migration Example: From Dict to Struct

As your code matures, you might start with dictionaries and migrate to structs:

```naab
use io
use json

// Phase 1: Start with dictionary (prototype)
fn process_user_dict(user: dict<string, any>) {
    io.write("Processing: ", user["name"], "\n")
    let age = user["age"]  // No type safety!
}

// Phase 2: Migrate to struct (production)
struct User {
    name: string
    age: int
    email: string
}

fn process_user_struct(user: User) {
    io.write("Processing: ", user.name, "\n")
    let age = user.age  // Type-safe!
}

main {
    // Dictionary version (for JSON parsing)
    let json_str = "{\"name\": \"Alice\", \"age\": 30, \"email\": \"alice@example.com\"}"
    let user_dict = json.parse(json_str)

    // Struct version (for application logic)
    let user_struct = new User {
        name: "Alice",
        age: 30,
        email: "alice@example.com"
    }

    process_user_dict(user_dict)
    process_user_struct(user_struct)
}
```

#### Common Mistake: Mixing Syntax

❌ **Don't mix struct and dictionary syntax:**
```naab
struct Person { name: string, age: int }

main {
    let person = new Person { name: "Alice", age: 30 }

    // ERROR: Can't use bracket notation on structs
    io.write(person["name"])  // WRONG!

    let data = {"name": "Bob", "age": 25}

    // ERROR: Can't use dot notation on dictionaries
    io.write(data.name)  // WRONG!
}
```

✅ **Use the right syntax for each type:**
```naab
struct Person { name: string, age: int }

main {
    let person = new Person { name: "Alice", age: 30 }
    io.write(person.name)  // ✅ Dot notation for structs

    let data = {"name": "Bob", "age": 25}
    io.write(data["name"])  // ✅ Bracket notation for dicts
}
```

**Verification:** See `docs/book/verification/ch02_basics/structs_vs_dicts.naab` for a complete working example.

## 2.5 Enums

Enums (enumerations) allow you to define a type by enumerating its possible values. They are useful for representing a fixed set of states or options.

```naab
enum Status {
    Pending,
    Active,
    Completed,
    Failed
}

struct Task {
    id: int,
    status: Status
}

main {
    let task = new Task {
        id: 1,
        status: Status.Pending
    }

    if task.status == Status.Pending {
        print("Task is pending")
        task.status = Status.Active
    }
}
```

## 2.6 Advanced Types: Generics and Null Safety

NAAb is designed with advanced type system features that enhance code robustness and flexibility. While some aspects of these features are still under active development (as of NAAb v1.0), their syntax and design principles are already established.

### 2.6.1 Generics

Generics allow you to write flexible, reusable code that works with different types without sacrificing type safety. You can define generic structs and functions that operate on type parameters, which are specified when the struct or function is used.

```naab
// Define a generic Box struct
struct Box<T> {
    value: T;
}

// Define a generic identity function
fn identity<T>(x: T) -> T {
    return x
}

main {
    // These examples demonstrate the supported syntax.
    let int_box = new Box<int> { value: 123 }
    let string_box = new Box<string> { value: "generic" }
    print("Int Box:", int_box.value)

    // Type inference also works for generic functions
    let result_int = identity(42)
    let result_string = identity("test")
    print("Identity Int:", result_int)
}
```
**Generics Support:** NAAb supports defining generic structs and functions. You can instantiate generic structs explicitly (e.g., `new Box<int>`) or rely on type inference for generic function calls.

### 2.5.2 Null Safety

NAAb's type system is designed to be null-safe by default, preventing a common class of runtime errors known as "null pointer exceptions." This means that unless a type is explicitly marked as nullable, it cannot hold a `null` value.

*   **Non-nullable by Default**: A type like `string` or `int` is assumed to never be `null`.
*   **Nullable Types**: To allow a variable to hold `null`, you append a question mark (`?`) to its type (e.g., `string?`, `int?`).

```naab
main {
    let name: string = "Alice" // Cannot be null
    // name = null             // This would be a compile-time error in a null-safe build

    let optional_greeting: string? = null // Can be null
    print("Optional Greeting:", optional_greeting) // Output: Optional Greeting: null

    optional_greeting = "Hello!"
    print("Optional Greeting (set):", optional_greeting) // Output: Optional Greeting (set): Hello!

    // In a fully null-safe build, accessing members of a nullable type
    // without a null check would also be a compile-time error.
    // For example, optional_greeting.length would be an error if optional_greeting is string?
    // You would need to check:
    // if (optional_greeting != null) {
    //     print(optional_greeting.length)
    // }
}
```
**Current Status (from `PHASE_2_4_5_NULL_SAFETY.md`):** Null safety is designed and planned for NAAb. The type system will enforce non-nullable by default semantics. While some aspects are already recognized, full compile-time enforcement and language features like optional chaining (`?.`) are still under active implementation.

## 2.6 Scope and Shadowing

In NAAb, variables have a defined scope, which determines where they can be accessed in your program. NAAb uses block-level scoping, meaning a variable declared within a block (like `main` or an `if` statement) is only accessible within that block.

```naab
main {
    let outer_var = "I am outside" // Accessible throughout main

    if (true) {
        let inner_var = "I am inside the if block" // Only accessible here
        print("Inside if:", outer_var)
        print("Inside if:", inner_var)
    }

    // print("Outside if:", inner_var) // This would be a compile-time error: 'inner_var' not found

    let x = 10
    if (true) {
        let x = 20 // This `x` shadows the outer `x` within this block
        print("Inner x:", x) // Output: Inner x: 20
    }
    print("Outer x:", x) // Output: Outer x: 10
}
```
Shadowing allows you to declare a new variable with the same name in an inner scope. This new variable temporarily "hides" the outer variable, but the outer variable's value remains unchanged.
