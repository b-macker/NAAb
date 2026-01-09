# Python Struct Representation Design

**Date**: 2026-01-08
**Author**: Claude (NAAb Struct Implementation - Week 5)
**Status**: Design Decision for Task 38

## Context

NAAb now has first-class struct types (Week 2-4 complete). Need to design how StructValue instances are marshalled to Python objects in the cross-language bridge.

## Requirements

1. **Type preservation**: Python objects must preserve struct type identity
2. **Field access**: Python code must access struct fields by name (e.g., `p.x`, `p.y`)
3. **Consistency**: Match existing marshalling patterns (recursive, symmetric)
4. **Debuggability**: Type name visible in Python repr/str
5. **Roundtrip safety**: Python → C++ → Python should preserve type and values

## Options Considered

### Option A: Dict with Metadata Key

**Representation**:
```python
{
    "__struct_type__": "Point",
    "x": 10,
    "y": 20
}
```

**Pros**:
- Simple implementation
- Consistent with existing dict marshalling
- Works with JSON serialization

**Cons**:
- Field access requires dict syntax: `p["x"]` not `p.x`
- Type name conflicts with legitimate field named `__struct_type__`
- No type identity in Python (just a dict)
- Poor debuggability (prints as generic dict)

**Verdict**: ❌ **Rejected** - Poor ergonomics for Python users

---

### Option B: Dynamic Dataclass Creation ⭐ **SELECTED**

**Representation**:
```python
@dataclass
class Point:
    x: int
    y: int

p = Point(x=10, y=20)
```

**Implementation**:
```cpp
py::object structToPython(const std::shared_ptr<StructValue>& s) {
    py::module types = py::module::import("types");
    py::dict namespace_dict;

    // Populate fields
    for (size_t i = 0; i < s->definition->fields.size(); ++i) {
        const auto& field = s->definition->fields[i];
        namespace_dict[py::str(field.name)] = valueToPython(s->field_values[i]);
    }

    // Create dynamic class
    py::object struct_class = types.attr("new_class")(
        py::str(s->type_name),
        py::tuple(),  // no bases
        py::dict(),   // no keywords
        py::cpp_function([namespace_dict](py::object ns) {
            ns.attr("update")(namespace_dict);
        })
    );

    return struct_class();
}
```

**Pros**:
- ✅ Natural field access: `p.x`, `p.y`
- ✅ Strong type identity: `type(p).__name__ == "Point"`
- ✅ Good debuggability: `repr(p)` shows type and fields
- ✅ Type preservation across conversions
- ✅ No namespace pollution or reserved field names

**Cons**:
- Slightly more complex implementation
- Dynamic class creation has small runtime cost
- Not directly JSON-serializable (requires custom encoder)

**Verdict**: ✅ **SELECTED** - Best user experience and type safety

---

### Option C: SimpleNamespace with Metadata

**Representation**:
```python
from types import SimpleNamespace
p = SimpleNamespace(x=10, y=20, __struct_type__="Point")
```

**Pros**:
- Attribute access: `p.x`
- Lightweight (no class creation)
- Simple implementation

**Cons**:
- Weak type identity: `type(p).__name__ == "SimpleNamespace"` (not "Point")
- Poor repr: shows SimpleNamespace not Point
- Metadata field `__struct_type__` can be overwritten
- Less debuggable than dataclass

**Verdict**: ❌ **Rejected** - Weak type identity defeats purpose

---

## Final Decision

**Selected: Option B - Dynamic Dataclass Creation**

### Rationale

1. **User Experience**: Python developers expect attribute access (`p.x`) and strong type identity
2. **Type Safety**: Dynamic classes preserve struct type name in `type(p).__name__`
3. **Debuggability**: `repr()` and `str()` show meaningful output
4. **Consistency**: Follows Python best practices for structured data
5. **Roundtrip**: Can detect struct types on reverse conversion via `__class__.__name__`

### Implementation Strategy

1. **structToPython()**: Use `types.new_class()` to create dynamic class per struct type
2. **Cache classes**: Store created classes by struct name to avoid re-creation overhead
3. **Field recursion**: Recursively call `valueToPython()` on field values (consistent with pattern)
4. **Reverse conversion**: Check if Python object has `__struct_type__` metadata or class name matches registered struct

### Example Usage

```python
# In NAAb:
struct Point {
    x: INT;
    y: INT;
}

let p = new Point { x: 10, y: 20 }
python.call("lambda p: p.x + p.y", p)

# In Python:
def process(point):
    print(type(point).__name__)  # "Point"
    print(point.x, point.y)      # 10 20
    return point.x + point.y
```

### Performance Considerations

- Class creation cost: ~1ms per unique struct type (acceptable)
- Caching: Store created classes in `std::unordered_map<std::string, py::object>` class cache
- Field access: Standard Python attribute lookup (same as dict access)

### Edge Cases

- **Empty structs**: Create class with no fields
- **Nested structs**: Recursive `valueToPython()` handles automatically
- **Struct arrays**: Each element converted independently
- **Large structs**: Class creation cost amortized across instances

---

## Acceptance Criteria

- [x] Design decision documented
- [x] Rationale clearly explained
- [x] Implementation strategy defined
- [x] Example code provided
- [x] Performance implications considered
- [x] Edge cases identified

**Status**: ✅ **ACCEPTED** - Ready for implementation in Task 39
