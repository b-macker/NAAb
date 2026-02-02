# Range Operator (`..`)

**Status:** ✅ Production Ready (Implemented 2026-01-22)
**Phase:** 3 - Essential Language Features
**Syntax:** `start..end`

## Overview

The range operator (`..`) provides a concise syntax for creating numeric ranges, commonly used in for-loop iteration. It generates a sequence of integers from `start` (inclusive) to `end` (exclusive), similar to Python's `range()` function.

## Syntax

```naab
start..end
```

- **start**: Starting value (inclusive) - any integer expression
- **end**: Ending value (exclusive) - any integer expression
- **Result**: Generates integers [start, start+1, ..., end-1]

## Basic Usage

### Simple Range Iteration

```naab
# Iterate from 0 to 9
for i in 0..10 {
    print(i)
}
# Output: 0 1 2 3 4 5 6 7 8 9
```

### Non-Zero Start

```naab
# Iterate from 10 to 14
for i in 10..15 {
    print(i)
}
# Output: 10 11 12 13 14
```

### Empty Range

```naab
# When start == end, no iterations occur
for i in 5..5 {
    print("This never executes")
}
```

## Advanced Usage

### Range with Variables

```naab
let start = 5
let end = 10

for i in start..end {
    print(i)
}
# Output: 5 6 7 8 9
```

### Range with Arithmetic Expressions

```naab
# Expressions are evaluated once before iteration
for i in 2*3..2*5 {
    print(i)
}
# Output: 6 7 8 9
```

### Accumulation Pattern

```naab
# Calculate sum of 1 to 100
let sum = 0
for i in 1..101 {
    sum = sum + i
}
print("Sum:", sum)  # Output: Sum: 5050
```

### Nested Ranges

```naab
# Generate coordinate pairs
for x in 0..3 {
    for y in 0..3 {
        print("(", x, ",", y, ")")
    }
}
# Output: (0,0) (0,1) (0,2) (1,0) (1,1) ...
```

### Range with Break/Continue

```naab
# Find first multiple of 7 after 20
for i in 20..100 {
    if i % 7 == 0 {
        print("Found:", i)
        break
    }
}

# Print only odd numbers
for i in 0..10 {
    if i % 2 == 0 {
        continue
    }
    print(i, "is odd")
}
```

### Building Lists from Ranges

```naab
# Create a list of squares
let squares = []
for i in 1..11 {
    squares = squares + [i * i]
}
print(squares)  # [1, 4, 9, 16, 25, 36, 49, 64, 81, 100]
```

## Common Patterns

### Countdown (Reverse Iteration)

Currently, ranges only support forward iteration. For countdown patterns, use a while loop:

```naab
# Countdown from 10 to 1
let i = 10
while i >= 1 {
    print(i)
    i = i - 1
}
```

### Step/Stride (Every Nth Element)

For custom step sizes, use conditional logic:

```naab
# Every 3rd number from 0 to 30
for i in 0..31 {
    if i % 3 == 0 {
        print(i)
    }
}
# Output: 0 3 6 9 12 15 18 21 24 27 30
```

## Operator Precedence

The range operator (`..`) has precedence between equality and comparison operators:

```naab
# Parentheses for clarity with complex expressions
for i in (a + b)..(c * d) {
    # ...
}

# Comparison operators bind tighter
let in_range = x >= 0..10  # ERROR: Parsed as (x >= 0)..10
let in_range = x >= (0..10)  # Use parentheses
```

## Performance

- **Lazy Evaluation**: Ranges are lightweight - no list is pre-generated
- **Memory**: O(1) space - only stores start and end values
- **Iteration**: O(n) time where n = end - start
- **Benchmarking**: Use ranges to test loop performance

Example benchmark:

```naab
use time as time

let start = time.now_millis()

let sum = 0
for i in 0..1000000 {
    sum = sum + 1
}

let elapsed = time.now_millis() - start
print("1M iterations:", elapsed, "ms")
```

## Implementation Details

### Internal Representation

Ranges are represented internally as dictionaries with special markers:

```
{
    "__is_range": true,
    "__range_start": <start_value>,
    "__range_end": <end_value>
}
```

This allows ranges to be distinguished from regular dictionaries during for-loop iteration without requiring a separate Value type.

### Type System

- Range expressions return type `Any` (will be `Range` in future type system)
- Start and end expressions must evaluate to integers
- Runtime error if non-integer values used

## Comparison with Other Languages

| Language | Syntax | End | Example |
|----------|--------|-----|---------|
| **NAAb** | `start..end` | Exclusive | `for i in 0..5` → 0,1,2,3,4 |
| Python | `range(start, end)` | Exclusive | `for i in range(0, 5)` → 0,1,2,3,4 |
| Rust | `start..end` | Exclusive | `for i in 0..5` → 0,1,2,3,4 |
| Ruby | `start...end` | Exclusive | `(0...5).each` → 0,1,2,3,4 |
| Go | N/A | N/A | `for i := 0; i < 5; i++` |

NAAb's range syntax follows Rust's convention: `..` for exclusive end.

## Future Enhancements

Potential future additions (not yet implemented):

- **Inclusive ranges**: `start..=end` (includes end value)
- **Reverse iteration**: `10..0` or `10 downto 0`
- **Step/stride**: `0..100 step 5`
- **Float ranges**: `0.0..1.0 step 0.1`
- **Range type**: Explicit `Range` type in type system
- **Range methods**: `range.contains(x)`, `range.length()`, `range.to_list()`

## Testing

Comprehensive test suite: `test_range_operator.naab`

Tests cover:
- Basic iteration (0..5)
- Non-zero start (10..15)
- Accumulation (sum of 1..10 = 55)
- Variable expressions
- Arithmetic expressions (2*3..2*5)
- Nested ranges
- Break/continue support
- Empty ranges (5..5)
- List building from ranges

All tests passing as of 2026-01-22.

## See Also

- [For Loops](FOR_LOOPS.md)
- [Control Flow](CONTROL_FLOW.md)
- [Time Module](TIME_MODULE.md) - For benchmarking with ranges
