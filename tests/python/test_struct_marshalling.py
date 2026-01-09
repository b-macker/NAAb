#!/usr/bin/env python3
"""
Python Struct Marshalling Tests
Week 5, Task 42 - Test struct ↔ Python object conversion

Tests the cross-language bridge's ability to marshal NAAb structs to/from Python objects.
"""

import sys
import os

# Add build directory to path to import C++ extension module
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../build'))

try:
    import naab_python  # C++ extension module (requires pybind11 bindings)
    NAAB_PYTHON_AVAILABLE = True
except ImportError as e:
    print(f"⚠️  naab_python module not available: {e}")
    print("   This is expected if pybind11 bindings haven't been created yet.")
    print("   Skipping tests...")
    NAAB_PYTHON_AVAILABLE = False


def test_struct_to_python():
    """Test NAAb struct → Python object conversion"""
    if not NAAB_PYTHON_AVAILABLE:
        print("⊘ test_struct_to_python SKIPPED (module unavailable)")
        return

    print("Running test_struct_to_python...")

    # Evaluate NAAb code that creates a struct
    result = naab_python.eval("""
        struct Point {
            x: INT;
            y: INT;
        }

        new Point { x: 10, y: 20 }
    """)

    # Verify Python object has correct attributes
    assert hasattr(result, 'x'), "Python object should have 'x' attribute"
    assert hasattr(result, 'y'), "Python object should have 'y' attribute"
    assert result.x == 10, f"Expected x=10, got {result.x}"
    assert result.y == 20, f"Expected y=20, got {result.y}"

    # Verify type name
    type_name = type(result).__name__
    assert type_name == "Point", f"Expected type 'Point', got '{type_name}'"

    print("✓ structToPython works: NAAb struct correctly converted to Python object")
    print(f"  type(result).__name__ = '{type_name}'")
    print(f"  result.x = {result.x}, result.y = {result.y}")


def test_python_to_struct():
    """Test Python object → NAAb struct conversion"""
    if not NAAB_PYTHON_AVAILABLE:
        print("⊘ test_python_to_struct SKIPPED (module unavailable)")
        return

    print("Running test_python_to_struct...")

    # Create Python object with struct-like attributes
    class Point:
        def __init__(self):
            self.x = 30
            self.y = 40

    p = Point()

    # Pass to NAAb and verify (requires naab_python.call_with_struct API)
    # This would call a NAAb function that accepts a Point struct
    result = naab_python.call_with_struct("identity_point", p, "Point")

    # Verify round-trip preserved values
    assert hasattr(result, 'x'), "Round-tripped object should have 'x'"
    assert hasattr(result, 'y'), "Round-tripped object should have 'y'"
    assert result.x == 30, f"Expected x=30, got {result.x}"
    assert result.y == 40, f"Expected y=40, got {result.y}"

    print("✓ pythonToStruct works: Python object correctly converted to NAAb struct")
    print(f"  result.x = {result.x}, result.y = {result.y}")


def test_nested_struct_marshalling():
    """Test nested structs (struct containing another struct)"""
    if not NAAB_PYTHON_AVAILABLE:
        print("⊘ test_nested_struct_marshalling SKIPPED (module unavailable)")
        return

    print("Running test_nested_struct_marshalling...")

    result = naab_python.eval("""
        struct Point {
            x: INT;
            y: INT;
        }

        struct Line {
            start: Point;
            end: Point;
        }

        new Line {
            start: new Point { x: 0, y: 0 },
            end: new Point { x: 100, y: 200 }
        }
    """)

    # Verify nested structure
    assert hasattr(result, 'start'), "Line should have 'start' field"
    assert hasattr(result, 'end'), "Line should have 'end' field"
    assert hasattr(result.start, 'x'), "Nested Point should have 'x'"
    assert hasattr(result.start, 'y'), "Nested Point should have 'y'"
    assert result.start.x == 0
    assert result.start.y == 0
    assert result.end.x == 100
    assert result.end.y == 200

    print("✓ Nested struct marshalling works")
    print(f"  Line.start = Point(x={result.start.x}, y={result.start.y})")
    print(f"  Line.end = Point(x={result.end.x}, y={result.end.y})")


def test_struct_array_marshalling():
    """Test array of structs"""
    if not NAAB_PYTHON_AVAILABLE:
        print("⊘ test_struct_array_marshalling SKIPPED (module unavailable)")
        return

    print("Running test_struct_array_marshalling...")

    result = naab_python.eval("""
        struct Point {
            x: INT;
            y: INT;
        }

        [
            new Point { x: 1, y: 2 },
            new Point { x: 3, y: 4 },
            new Point { x: 5, y: 6 }
        ]
    """)

    # Verify array was converted to Python list
    assert isinstance(result, list), f"Expected list, got {type(result)}"
    assert len(result) == 3, f"Expected 3 elements, got {len(result)}"

    # Verify each element is a Point
    for i, point in enumerate(result):
        assert hasattr(point, 'x'), f"Element {i} should have 'x'"
        assert hasattr(point, 'y'), f"Element {i} should have 'y'"

    assert result[0].x == 1 and result[0].y == 2
    assert result[1].x == 3 and result[1].y == 4
    assert result[2].x == 5 and result[2].y == 6

    print("✓ Struct array marshalling works")
    print(f"  Array length: {len(result)}")
    for i, p in enumerate(result):
        print(f"  result[{i}] = Point(x={p.x}, y={p.y})")


if __name__ == "__main__":
    print("=" * 60)
    print("Python Struct Marshalling Tests")
    print("Week 5, Task 42")
    print("=" * 60)
    print()

    if not NAAB_PYTHON_AVAILABLE:
        print("⚠️  Tests cannot run: naab_python module not available")
        print()
        print("To enable these tests, create pybind11 bindings that:")
        print("  1. Expose naab_python.eval(code: str) -> Any")
        print("  2. Expose naab_python.call_with_struct(fn: str, obj: Any, type: str) -> Any")
        print("  3. Use CrossLanguageBridge for struct marshalling")
        print()
        sys.exit(0)

    try:
        test_struct_to_python()
        print()
        test_python_to_struct()
        print()
        test_nested_struct_marshalling()
        print()
        test_struct_array_marshalling()
        print()
        print("=" * 60)
        print("All Python struct marshalling tests passed! ✓")
        print("=" * 60)

    except AssertionError as e:
        print()
        print("=" * 60)
        print(f"✗ Test FAILED: {e}")
        print("=" * 60)
        sys.exit(1)

    except Exception as e:
        print()
        print("=" * 60)
        print(f"✗ Test ERROR: {e}")
        import traceback
        traceback.print_exc()
        print("=" * 60)
        sys.exit(1)
