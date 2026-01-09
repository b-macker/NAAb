# NAAb Struct User Guide

## Introduction

Structs in NAAb provide first-class composite data types with named fields. They enable type-safe data modeling and work seamlessly across all supported languages (Python, JavaScript, C++).

## Basic Usage

### Declaring Structs

```naab
struct Point {
    x: INT;
    y: INT;
}

struct Person {
    name: STRING;
    age: INT;
    email: STRING;
}
```

### Creating Struct Instances

Use the `new` keyword to create struct instances:

```naab
let p = new Point { x: 10, y: 20 }
let alice = new Person {
    name: "Alice",
    age: 30,
    email: "alice@example.com"
}
```

### Accessing Fields

```naab
print(p.x)        # Output: 10
print(alice.name) # Output: Alice
```

## Advanced Features

### Nested Structs

```naab
struct Line {
    start: Point;
    end: Point;
}

let line = new Line {
    start: new Point { x: 0, y: 0 },
    end: new Point { x: 100, y: 200 }
}

print(line.start.x)  # Output: 0
```

### Structs in Collections

**Arrays of structs**:
```naab
let points = [
    new Point { x: 1, y: 2 },
    new Point { x: 3, y: 4 },
    new Point { x: 5, y: 6 }
]
```

**Maps with struct values**:
```naab
let locations = {
    home: new Point { x: 0, y: 0 },
    work: new Point { x: 10, y: 20 }
}
```

## Cross-Language Usage

### Python Integration

```naab
struct Data { value: INT; }

main {
    let d = new Data { value: 42 }
    # Python receives struct as object with attributes
    python.call("lambda d: d.value * 2", d)
}
```

Python sees:
```python
# d.value == 42
# type(d).__name__ == "Data"
```

### JavaScript Integration

```naab
struct Config { port: INT; }

main {
    let cfg = new Config { port: 8080 }
    # JavaScript receives struct as object
    js.eval("(function(c) { return c.port; })", cfg)
}
```

JavaScript sees:
```javascript
// cfg.port === 8080
// cfg.__struct_type__ === "Config"
```

### C++ Block Integration

C++ blocks can use the struct API:

```cpp
void* naab_value_create_struct(const char* type_name);
void* naab_value_get_struct_field(void* value, const char* field_name);
int naab_value_set_struct_field(void* struct_value, const char* field_name, void* field_value);
```

## Module System

### Exporting Structs

```naab
# point_module.naab
export struct Point {
    x: INT;
    y: INT;
}
```

### Importing Structs

```naab
# main.naab
import * as point from "./point_module.naab"

main {
    let p = new Point { x: 5, y: 10 }
}
```

## Best Practices

1. **Use descriptive names**: `UserProfile` not `UP`
2. **Group related fields**: Design cohesive structs
3. **Avoid deep nesting**: More than 2-3 levels makes code hard to read
4. **Document fields**: Use comments for complex types

## Performance

Struct operations are highly optimized:
- Field access: O(1) hash lookup
- Memory overhead: ~10% vs plain maps
- Cross-language marshalling: Zero-copy where possible

## Troubleshooting

### "Undefined struct" error

```naab
# Error: Undefined struct: Point
let p = new Point { x: 1, y: 2 }
```

**Solution**: Declare the struct first or import it:
```naab
struct Point { x: INT; y: INT; }
let p = new Point { x: 1, y: 2 }
```

### "Missing required field" error

```naab
struct Point { x: INT; y: INT; }
let p = new Point { x: 10 }  # Error: missing field 'y'
```

**Solution**: Provide all required fields:
```naab
let p = new Point { x: 10, y: 0 }
```

### Forgot 'new' keyword

```naab
# This creates a map literal, not a struct!
let p = Point { x: 10, y: 20 }
```

**Solution**: Always use `new` for struct literals:
```naab
let p = new Point { x: 10, y: 20 }
```

## Examples

### Complete Example: Geometric Shapes

```naab
struct Point {
    x: INT;
    y: INT;
}

struct Rectangle {
    top_left: Point;
    width: INT;
    height: INT;
}

function calculate_area(rect: Rectangle) -> INT {
    return rect.width * rect.height
}

main {
    let r = new Rectangle {
        top_left: new Point { x: 0, y: 0 },
        width: 100,
        height: 50
    }

    let area = calculate_area(r)
    print(area)  # Output: 5000
}
```

## See Also

- [Grammar Reference](./grammar.md) - Formal struct syntax
- [API Reference](./API_REFERENCE.md) - C++ block interface for structs
- [Architecture](./ARCHITECTURE.md) - Struct implementation details
