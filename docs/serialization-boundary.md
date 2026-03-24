# Serialization Boundary: NAAb ↔ Python

What happens when data crosses the NAAb→Python→NAAb boundary via polyglot blocks.

## Architecture

```
NAAb Value → serializeValueForLanguage() → Python source code string
                                            ↓
                                     Python C API executes
                                            ↓
Python PyObject* → pyObjectToValue() → NAAb Value
```

**Key files:**
- `src/interpreter/polyglot.cpp:1808` — `serializeValueForLanguage()`
- `src/runtime/python_c_executor.cpp:348` — `pyObjectToValue()`
- `src/runtime/python_c_executor.cpp:94` — `executeWithReturn()`

## Supported Types (Full Roundtrip)

| NAAb Type | Python Serialization | Python → NAAb | Verified |
|-----------|---------------------|---------------|----------|
| `int` | `42` | `PyLong` → int (or double if >INT32) | Yes |
| `double` | `3.14159265358979` (%.15g) | `PyFloat` → double | Yes |
| `bool` | `True` / `False` | `PyBool` → bool (checked before int) | Yes |
| `string` | `"escaped\"string"` | `PyUnicode` → string | Yes |
| `null` (monostate) | `None` | `Py_None` → monostate | Yes |
| `list` | `[1, 2, 3]` | `PyList` → vector | Yes |
| `dict` | `{"key": "value"}` | `PyDict` → unordered_map | Yes |
| `struct` | `{"field": value}` (JSON object) | N/A (becomes dict) | Yes |

## String Edge Cases

| Case | Status | Notes |
|------|--------|-------|
| ASCII | Pass | Standard escaping: `"`, `\`, `\n`, `\r`, `\t`, `\0` |
| Unicode (emoji) | Pass | `"hello 🌍"` roundtrips correctly |
| Unicode (CJK) | Pass | `"日本語テスト"` roundtrips correctly |
| Unicode (Arabic) | Pass | `"مرحبا"` roundtrips correctly |
| Embedded quotes | Pass | `"he said \"hi\""` — escaped in serialization |
| Backslashes | Pass | `"path\\to\\file"` — double-escaped |
| Newlines | Pass | `"line1\nline2"` — escaped as `\n` |
| Tabs | Pass | `"a\tb"` — escaped as `\t` |
| Empty string | Pass | `""` roundtrips correctly |
| Whitespace-only | Pass | `"   "` preserved |
| 1MB string | Pass | No crash, no truncation |
| **Null bytes** | **Known limit** | C string boundary may truncate at `\0` |

## Collection Edge Cases

| Case | Status | Notes |
|------|--------|-------|
| Empty list `[]` | Pass | |
| Nested 5 levels | Pass | `[[[[[42]]]]]` |
| Nested 10 levels | Pass | No crash |
| 1000-element list | Pass | No crash or timeout |
| 500-entry dict | Pass | No crash or timeout |
| Mixed-type list | Pass | `[1, "hello", True, 3.14]` |
| Dict-in-list | Pass | `[{"x": 1}, {"y": 2}]` |
| List-in-dict | Pass | `{"nums": [1, 2, 3]}` |
| Nested dict 5 levels | Pass | |
| Mixed nesting | Pass | Users array with nested scores |

## Dict Key Edge Cases

| Key Content | Status | Notes |
|-------------|--------|-------|
| Numeric-like (`"123"`) | Pass | Stays as string key |
| Empty (`""`) | Pass | |
| Contains `"` | Pass | Escaped via `escapeKey()` |
| Contains `\` | Pass | Escaped via `escapeKey()` |
| Contains `\n` | Pass | Escaped via `escapeKey()` |

## Python → NAAb Type Coercion

| Python Type | NAAb Result | Status |
|-------------|-------------|--------|
| `int` (fits int32) | `int` | Pass |
| `int` (>INT32_MAX) | `double` | Pass — precision loss for >2^53 |
| `float` | `double` | Pass |
| `bool` | `bool` | Pass — checked before int (Python bool subclasses int) |
| `str` | `string` | Pass |
| `None` | `null` (monostate) | Pass |
| `list` | `list` | Pass — recursive conversion |
| `tuple` | `list` | Pass — flattened to list |
| `dict` | `dict` | Pass — keys must be strings |
| **`set`** | **Known limit** | Wrapped as PythonObjectValue |
| **`bytes`** | **Known limit** | Wrapped as PythonObjectValue |
| **`complex`** | **Known limit** | Wrapped as PythonObjectValue |

## Unsupported NAAb → Python Types

These NAAb types serialize as `null` when bound to Python:

| NAAb Type | Serialization | Reason |
|-----------|---------------|--------|
| `FunctionValue` | `null` | No callable marshaling across boundary |
| `GeneratorValue` | `null` | No iterator protocol marshaling |
| `FutureValue` | `null` | No async marshaling |
| `BlockValue` | `null` | Internal runtime type |
| `PythonObjectValue` | `null` | Already a Python object (no re-serialization path) |

## Behavioral Notes

- **Copy semantics**: Python receives a copy. Mutations in Python do not affect NAAb values.
- **Dict key ordering**: `unordered_map` means serialization order is non-deterministic. Tests use `sorted()` for comparison.
- **Float precision**: Uses `%.15g` format (not `std::to_string` which gives 6 decimals). Matches Python's `repr()` for most values.
- **Shadowing Python builtins**: Binding a NAAb variable named `len` overwrites Python's `len()` in that execution scope.
- **Struct flattening**: NAAb structs become Python dicts. Field names become string keys. Type information is lost.

## Test Coverage

Automated tests in `tests/property/test_serialization_audit.sh`:
- 40 roundtrip/crash tests (all pass)
- 6 known limitations (documented)
- 0 unexpected failures
