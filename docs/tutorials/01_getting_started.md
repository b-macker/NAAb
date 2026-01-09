# Tutorial 1: Getting Started with NAAb

**Duration**: 15 minutes
**Prerequisites**: None
**What you'll learn**: Install NAAb, write your first program, use the REPL

---

## Step 1: Installation

### Build NAAb

```bash
cd /storage/emulated/0/Download/.naab/naab_language

# Configure build
cmake -B build

# Compile (takes ~2-3 minutes)
cmake --build build -j4
```

### Install Binaries

```bash
# Copy to home directory for easy access
cp build/naab-lang ~/
cp build/naab-repl ~/

# Verify installation
~/naab-lang version
```

Expected output:
```
NAAb v0.1.0
Git: 54b4a3e
Build: 20251230
```

---

## Step 2: Hello, World!

Create your first NAAb program:

```bash
cat > hello.naab << 'EOF'
print("Hello, World!")
EOF
```

Run it:

```bash
~/naab-lang run hello.naab
```

Output:
```
Hello, World!
```

**Congratulations!** You've written your first NAAb program.

---

## Step 3: Variables and Math

Create `calculator.naab`:

```naab
// Variables
let x = 10
let y = 5

// Arithmetic
let sum = x + y
let difference = x - y
let product = x * y
let quotient = x / y

// Output
print("Sum: " + sum)
print("Difference: " + difference)
print("Product: " + product)
print("Quotient: " + quotient)
```

Run it:

```bash
~/naab-lang run calculator.naab
```

Output:
```
Sum: 15
Difference: 5
Product: 50
Quotient: 2
```

---

## Step 4: Using the REPL

Start the interactive REPL:

```bash
~/naab-repl
```

Try these commands:

```naab
> let name = "Alice"
> print("Hello, " + name)
Hello, Alice

> let numbers = [1, 2, 3, 4, 5]
> print(numbers)
[1, 2, 3, 4, 5]

> let doubled = []
> for (n in numbers) {
>     doubled = doubled + [n * 2]
> }
> print(doubled)
[2, 4, 6, 8, 10]

> exit
```

---

## Step 5: Functions

Create `greetings.naab`:

```naab
// Define function
function greet(name) {
    return "Hello, " + name + "!"
}

// Call function
let message1 = greet("Alice")
let message2 = greet("Bob")

print(message1)
print(message2)
```

Run it:

```bash
~/naab-lang run greetings.naab
```

Output:
```
Hello, Alice!
Hello, Bob!
```

### Functions with Default Parameters

```naab
function greet(name = "World") {
    return "Hello, " + name + "!"
}

print(greet())          // "Hello, World!"
print(greet("Alice"))   // "Hello, Alice!"
```

---

## Step 6: Working with Arrays

Create `arrays.naab`:

```naab
import "stdlib" as std

// Create array
let numbers = [10, 20, 30, 40, 50]

// Array operations
print("Length: " + std.length(numbers))
print("First: " + std.first(numbers))
print("Last: " + std.last(numbers))

// Add element
numbers = std.push(numbers, 60)
print("After push: " + numbers)

// Reverse
let reversed = std.reverse(numbers)
print("Reversed: " + reversed)

// Join to string
let joined = std.join(numbers, ", ")
print("Joined: " + joined)
```

Run it:

```bash
~/naab-lang run arrays.naab
```

---

## Step 7: Control Flow

Create `conditions.naab`:

```naab
let age = 25

if (age >= 18) {
    print("You are an adult")
} else {
    print("You are a minor")
}

// Loop
let count = 0
while (count < 5) {
    print("Count: " + count)
    count = count + 1
}

// For loop
let fruits = ["apple", "banana", "orange"]
for (fruit in fruits) {
    print("Fruit: " + fruit)
}
```

Run it:

```bash
~/naab-lang run conditions.naab
```

---

## Step 8: Error Handling

Create `errors.naab`:

```naab
function divide(a, b) {
    if (b == 0) {
        throw "Cannot divide by zero"
    }
    return a / b
}

try {
    let result = divide(10, 2)
    print("10 / 2 = " + result)

    let bad_result = divide(10, 0)
} catch (error) {
    print("Error caught: " + error)
} finally {
    print("Done with division")
}
```

Run it:

```bash
~/naab-lang run errors.naab
```

Output:
```
10 / 2 = 5
Error caught: Cannot divide by zero
Done with division
```

---

## Next Steps

You now know the basics of NAAb! Continue with:

- **Tutorial 2**: [Building Your First Application](02_first_application.md)
- **Tutorial 3**: [Multi-File Applications](03_multi_file_apps.md)
- **User Guide**: [../USER_GUIDE.md](../USER_GUIDE.md)
- **API Reference**: [../API_REFERENCE.md](../API_REFERENCE.md)

---

## Quick Reference

### Common Commands

```bash
~/naab-lang run <file>          # Run a NAAb program
~/naab-lang parse <file>        # Parse and show AST
~/naab-lang check <file>        # Type check program
~/naab-repl                     # Start interactive REPL
~/naab-lang version             # Show version
~/naab-lang help                # Show all commands
```

### Common Patterns

```naab
// Variables
let x = 42

// Functions
function add(a, b) {
    return a + b
}

// Arrays
let nums = [1, 2, 3]

// Loops
for (item in array) {
    print(item)
}

// Error handling
try {
    risky_operation()
} catch (error) {
    handle_error(error)
}
```

---

**Congratulations!** You've completed the Getting Started tutorial.
