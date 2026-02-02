# Chapter 8: Strings, Text, and Math

NAAb's native Standard Library provides robust modules for handling common data types like strings and numbers. This chapter explores the `string` module for text manipulation, the `regex` module for powerful pattern matching, and the `math` module for various mathematical operations. All these modules are implemented in high-performance C++, making them significantly faster than equivalent polyglot solutions.

## 8.1 The `string` Module: Manipulation and Parsing

The `string` module (commonly aliased as `str`) offers a comprehensive set of functions for working with text. From basic case conversions to splitting and joining, these functions streamline common string operations.

```naab
use string as str // Import the string module

main {
    let message = "  Hello, NAAb World!  "
    print("Original message:", message)

    // Basic manipulation
    let trimmed = str.trim(message)
    print("Trimmed:", trimmed) // Output: "Hello, NAAb World!"

    let upper = str.upper(trimmed)
    print("Uppercase:", upper) // Output: "HELLO, NAAB WORLD!"

    let lower = str.lower(upper)
    print("Lowercase:", lower) // Output: "hello, naab world!"

    // Length
    print("Length:", str.length(message)) // Output: 22

    // Splitting and Joining
    let csv_data = "apple,banana,cherry"
    let fruits = str.split(csv_data, ",")
    print("Split fruits:", fruits) // Output: ["apple", "banana", "cherry"]

    let joined_data = str.join(fruits, " | ")
    print("Joined data:", joined_data) // Output: "apple | banana | cherry"

    // Substring and Indexing
    let full_string = "NAAb Programming"
    let sub = str.substring(full_string, 5, 8) // Get "Pro"
    print("Substring 'Pro':", sub) // Output: Pro

    let index = str.index_of(full_string, "Program")
    print("Index of 'Program':", index) // Output: 5 (0-indexed)

    // Repetition
    let repeated = str.repeat("NAAb", 3)
    print("Repeated 'NAAb':", repeated) // Output: NAAbNAAbNAAb
}
```

## 8.2 The `regex` Module: Pattern Matching

The `regex` module provides powerful tools for defining and applying regular expressions to text. This enables sophisticated pattern matching, searching, and replacement capabilities.

```naab
use regex as re // Import the regex module

main {
    let text = "The quick brown fox jumps over the lazy dog."
    let pattern = "fox|dog"

    // Check if pattern matches anywhere in the string (use 'search' for partial, 'match' for full)
    let matches_any = re.search(text, pattern)
    print("Text matches 'fox|dog':", matches_any) // Output: true

    // Find all occurrences
    let all_found = re.find_all(text, "[a-z]+") // Find all words
    print("All words:", all_found) // Output: ["The", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog"]

    // Replace occurrences
    let replaced_text = re.replace(text, "fox", "cat")
    print("Replaced 'fox' with 'cat':", replaced_text) // Output: "The quick brown cat jumps over the lazy dog."

    // Check for a full match (from start to end of string)
    let full_match_test = re.matches("NAAb", "^NAAb$")
    print("'NAAb' fully matches '^NAAb$':", full_match_test) // Output: true

    let no_full_match = re.matches("NAAb Language", "^NAAb$")
    print("'NAAb Language' fully matches '^NAAb$':", no_full_match) // Output: false
}
```
**Note:** The `re.find_all` function takes an optional flag for case-insensitivity or other regex options.

## 8.3 The `math` Module: Advanced Calculation

The `math` module (aliased as `math`) provides a collection of mathematical functions and constants, including trigonometric functions, powers, roots, and absolute values.

```naab
use math as math // Import the math module

main {
    // Basic arithmetic functions
    print("Absolute value of -10:", math.abs(-10)) // Output: 10
    print("Square root of 25:", math.sqrt(25.0))   // Output: 5.0

    // Powers
    print("2 to the power of 8:", math.pow(2.0, 8.0)) // Output: 256.0

    // Rounding and truncation
    print("Floor of 3.7:", math.floor(3.7)) // Output: 3.0
    print("Ceil of 3.2:", math.ceil(3.2))   // Output: 4.0
    print("Round of 3.4:", math.round(3.4)) // Output: 3.0
    print("Round of 3.8:", math.round(3.8)) // Output: 4.0

    // Min and Max
    print("Min of (10, 20):", math.min(10, 20)) // Output: 10
    print("Max of (10, 20):", math.max(10, 20)) // Output: 20

    // Mathematical constants
    print("Value of PI:", math.PI) // Output: 3.14159...
    print("Value of E:", math.E)   // Output: 2.71828...
}
```
**Note:** Many mathematical functions operate on `float` types. Ensure you pass floating-point literals (e.g., `25.0` instead of `25`) where appropriate to avoid implicit conversions or function mismatches.
