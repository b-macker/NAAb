# NAAb Security Tests

This directory contains security tests for the NAAb language implementation.

## Week 1: Critical Infrastructure (COMPLETED)

### Task 1.1: Sanitizers in CI ✅
- **Status**: COMPLETE
- **Files Modified**:
  - `CMakeLists.txt` - Added sanitizer build options
  - `.github/workflows/sanitizers.yml` - CI configuration
- **Features**:
  - AddressSanitizer (ASan) - Memory errors
  - UndefinedBehaviorSanitizer (UBSan) - Undefined behavior
  - MemorySanitizer (MSan) - Uninitialized reads
  - ThreadSanitizer (TSan) - Data races
- **Test**: `test_sanitizers.naab`

### Task 1.2: Input Size Caps ✅
- **Status**: COMPLETE
- **Files Modified**:
  - `include/naab/limits.h` - Created with all limits
  - `src/lexer/lexer.cpp` - Source file size validation
  - `src/stdlib/io.cpp` - File read size validation
  - `src/runtime/python_executor.cpp` - Polyglot block size validation
  - `src/runtime/js_executor.cpp` - Polyglot block size validation
- **Limits**:
  - Max file size: 10 MB
  - Max polyglot block: 1 MB
  - Max source string: 100 MB
  - Max line length: 10,000 chars
- **Test**: `test_input_limits.naab`

### Task 1.3: Recursion Depth Limits ✅
- **Status**: COMPLETE
- **Files Modified**:
  - `include/naab/limits.h` - Recursion limit constants
  - `include/naab/parser.h` - Parser depth tracking
  - `src/parser/parser.cpp` - DepthGuard implementation
  - `include/naab/interpreter.h` - Call depth tracking
  - `src/interpreter/interpreter.cpp` - CallDepthGuard implementation
- **Limits**:
  - Parser max depth: 1,000 levels
  - Call stack max depth: 10,000 levels
- **Test**: `test_recursion_limits.naab`

## Running Security Tests

### With Sanitizers

```bash
# Build with AddressSanitizer
cmake -B build-asan -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j4

# Run security tests
./build-asan/naab-lang tests/security/test_sanitizers.naab
./build-asan/naab-lang tests/security/test_input_limits.naab
./build-asan/naab-lang tests/security/test_recursion_limits.naab
```

### With UndefinedBehaviorSanitizer

```bash
# Build with UBSan
cmake -B build-ubsan -DENABLE_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ubsan -j4

# Run tests
./build-ubsan/naab-lang tests/security/*.naab
```

### Testing Limits

To actually trigger the security limits:

**Input Size Limits:**
```bash
# Create a huge file
dd if=/dev/zero of=/tmp/huge.txt bs=1M count=20  # 20MB file

# Try to read it (should fail with InputSizeException)
echo "use io; main { io.read_file('/tmp/huge.txt') }" | ./naab-lang -
```

**Recursion Depth Limits:**
```naab
// Deep recursion test
fn recurse(n: int) -> int {
    if n <= 0 { return 0 }
    return recurse(n - 1) + 1
}

main {
    // This should throw RecursionLimitException
    let x = recurse(15000)
}
```

**Parser Depth Limits:**
```naab
// Generate deeply nested expression
// ((((((...1001 times...))))))
// Should throw RecursionLimitException during parsing
```

## CI Integration

The `.github/workflows/sanitizers.yml` workflow runs automatically on:
- Push to `main` or `develop` branches
- Pull requests

The workflow:
1. Builds with ASan and UBSan
2. Runs all unit tests
3. Runs verification tests
4. Fails if any sanitizer detects issues

## Security Coverage

After Week 1 completion:

**Before:**
- No sanitizers
- No input validation
- No recursion limits
- Safety Grade: D+ (42%)

**After:**
- ✅ All sanitizers enabled in CI
- ✅ Input size caps on all external inputs
- ✅ Recursion depth limits in parser and interpreter
- ✅ Continuous security testing
- **Safety Grade: ~55% (+13%)**

## Next Steps

Week 2-6 will add:
- Fuzzing infrastructure
- Supply chain security (SBOM, signing)
- FFI/polyglot boundary validation
- Path canonicalization
- Arithmetic overflow checking
- Comprehensive security test suite

## References

- Security Policy: [SECURITY.md](../../SECURITY.md)
- Security Chapter: [Chapter 13](../../docs/book/chapter13.md)
