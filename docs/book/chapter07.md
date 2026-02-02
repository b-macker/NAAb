# Chapter 7: Data Structures & Algorithms

NAAb's native C++ Standard Library (stdlib) provides highly optimized modules for common data structures and algorithms. Leveraging these modules is crucial for building performant NAAb applications, often yielding a 10-100x speed improvement compared to equivalent operations performed within polyglot blocks. This chapter explores the `array` and `collections` modules.

## 7.1 The `array` Module: Manipulating Ordered Collections

The `array` module (aliased as `arr` in common practice) provides a suite of functions for working with arrays (NAAb's ordered collections, often called lists). These functions operate on arrays efficiently, directly within the NAAb runtime.

### 7.1.1 Core Array Functions

The `array` module offers essential functionalities like sorting, reversing, checking for element presence, and obtaining length.

```naab
use array as arr // Import the array module

main {
    let numbers = [42, 17, 8, 91, 23, 56, 3]
    print("Original numbers:", numbers)

    // Sorting
    let sorted_numbers = arr.sort(numbers)
    print("Sorted numbers:", sorted_numbers) // Output: [3, 8, 17, 23, 42, 56, 91]

    // Reversing
    let reversed_numbers = arr.reverse(numbers)
    print("Reversed numbers:", reversed_numbers) // Output: [56, 23, 91, 8, 17, 42, 3]

    // Length
    let len = arr.length(numbers)
    print("Length of numbers:", len) // Output: 7

    // Contains (check for element presence)
    let has_42 = arr.contains(numbers, 42)
    let has_999 = arr.contains(numbers, 999)
    print("Contains 42:", has_42)   // Output: true
    print("Contains 999:", has_999) // Output: false
}
```

### 7.1.2 Higher-Order Array Functions: `map`, `filter`, `reduce`

For functional-style transformations and aggregations, the `array` module provides `map`, `filter`, and `reduce` functions, which take other functions as arguments.

*   **`arr.map(array, transform_fn)`**: Applies a `transform_fn` to each element of the array and returns a new array with the results.
*   **`arr.filter(array, predicate_fn)`**: Creates a new array containing only the elements for which the `predicate_fn` returns `true`.
*   **`arr.reduce(array, combine_fn, initial_value)`**: Applies a `combine_fn` to an accumulator and each element (from left to right) to reduce the array to a single value.

```naab
use array as arr

// Define helper functions
fn double(x: int) -> int {
    return x * 2
}

fn is_even(x: int) -> bool {
    return x % 2 == 0
}

fn sum_reducer(accumulator: int, current_value: int) -> int {
    return accumulator + current_value
}

main {
    let original_numbers = [1, 2, 3, 4, 5]
    print("Original numbers:", original_numbers)

    // Map: double each number
    let doubled = arr.map_fn(original_numbers, double)
    print("Doubled numbers:", doubled) // Output: [2, 4, 6, 8, 10]

    // Filter: keep only even numbers
    let evens = arr.filter_fn(original_numbers, is_even)
    print("Even numbers:", evens) // Output: [2, 4]

    // Reduce: sum all numbers
    let total_sum = arr.reduce_fn(original_numbers, sum_reducer, 0)
    print("Total sum:", total_sum) // Output: 15
}
```

## 7.2 The `collections` Module: Sets

The `collections` module provides advanced data structures beyond simple arrays and dictionaries. Currently, it includes a robust implementation of Set operations. Sets are unordered collections of unique elements.

### 7.2.1 Core Set Operations

The `collections` module (often aliased as `col` or `set`) offers functions to create, modify, and query sets.

```naab
use collections as col // Import the collections module

main {
    // Create new sets
    let set1 = col.Set()
    let set2 = col.Set()

    // Add elements (Sets are immutable, operations return new sets)
    set1 = col.set_add(set1, "apple")
    set1 = col.set_add(set1, "banana")
    set1 = col.set_add(set1, "apple") // Adding "apple" again has no effect (sets have unique elements)
    print("Set 1 after adds:", col.to_array(set1)) // to_array converts set to an array for printing

    set2 = col.set_add(set2, "banana")
    set2 = col.set_add(set2, "orange")
    print("Set 2 after adds:", col.to_array(set2))

    // Check size
    print("Size of Set 1:", col.set_size(set1)) // Output: 2

    // Check for element presence
    print("Set 1 contains 'apple':", col.set_contains(set1, "apple"))     // Output: true
    print("Set 1 contains 'grape':", col.set_contains(set1, "grape"))     // Output: false

    // Remove elements
    set1 = col.set_remove(set1, "banana")
    print("Set 1 after removing 'banana':", col.to_array(set1)) // Output: ["apple"] (order may vary)
}
```
**Note:** The `col.to_array()` function is an assumed utility for demonstration purposes. The `collections` module implements persistent/immutable sets, meaning `set_add` returns a new set rather than modifying the existing one in-place.

## 7.3 Performance: Native vs. Polyglot (The 10-100x Rule)

The primary reason to use NAAb's native Standard Library modules (like `array` and `collections`) over implementing similar logic in polyglot blocks is **performance**.

Native Standard Library functions are implemented in highly optimized C++ code, which runs directly within the NAAb runtime. This avoids the overheads associated with:

1.  **Language Bridging**: The process of marshaling data between NAAb and a foreign language's runtime.
2.  **Foreign Language Startup**: The time it takes to initialize a Python interpreter or a JavaScript engine for a block.
3.  **Dynamic Typing Overheads**: Many scripting languages (Python, JavaScript) have inherent performance costs due to dynamic type checking.

For tasks like sorting large arrays, filtering complex data structures, or performing repetitive string manipulations, using the native NAAb Standard Library can be **10 to 100 times faster** than calling into a Python or JavaScript block to do the same task.

**When to Use Which:**

*   **Native Stdlib**: For generic data structure manipulation, mathematical operations, and system interactions where performance is critical.
*   **Polyglot Blocks**: For leveraging specific domain-rich libraries (e.g., NumPy/Pandas in Python, specialized C++ libraries), or when you need side effects unique to a foreign language (e.g., system commands in Bash).

By intelligently choosing between native stdlib and polyglot blocks, you can build NAAb applications that are both powerful and incredibly efficient.
