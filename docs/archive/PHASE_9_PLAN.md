# Phase 9: Integration Test Execution

**Date**: December 17, 2025
**Status**: Ready to Execute
**Priority**: Critical (verify end-to-end functionality)

---

## Overview

Execute all 18 integration tests from INTEGRATION_TEST_PLAN.md to verify end-to-end functionality of the multi-language block assembly system.

**Primary Goal**: Verify that all components work together correctly from block loading to execution.

---

## Test Categories

From INTEGRATION_TEST_PLAN.md:

| Category | Tests | Status | Description |
|----------|-------|--------|-------------|
| 1. Executor Registration | 2 | ✅ VERIFIED | Executors initialize on startup |
| 2. REPL Commands | 3 | ✅ READY | Interactive commands work |
| 3. C++ Block Loading | 2 | ✅ READY | C++ blocks load and execute |
| 4. JS Block Loading | 2 | ✅ READY | JavaScript blocks work |
| 5. Polyglot Programs | 1 | ✅ READY | Multi-language programs |
| 6. Error Handling | 3 | ✅ READY | Graceful error reporting |
| 7. Code Verification | 3 | ✅ VERIFIED | Builds succeed |
| 8. Performance Tests | 2 | ✅ READY | Load/stress testing |
| **TOTAL** | **18** | **100% READY** | |

---

## Execution Plan

### Phase 1: Code Verification (Already Complete ✅)

**Status**: VERIFIED in Phase 7c

**Tests**:
1. ✅ Test 7.1: Build naab-lang
2. ✅ Test 7.2: Build naab-repl
3. ✅ Test 1.1: Executor registration verified (code inspection)
4. ✅ Test 1.2: Startup banner verified (code inspection)

**Result**: 4/4 tests passed

---

### Phase 2: Manual Smoke Tests

**Procedure**:

#### Test 1.1: naab-lang Version Command
```bash
./naab-lang version
```

**Expected Output**:
```
NAAb Block Assembly Language v0.1.0
Supported languages: cpp, javascript
```

**Pass Criteria**: Shows version and supported languages

---

#### Test 1.2: REPL Startup Banner
```bash
./naab-repl
```

**Expected Output**:
```
╔═══════════════════════════════════════════════════════╗
║  NAAb Block Assembly Language - Interactive Shell    ║
║  Version 0.1.0                                        ║
╚═══════════════════════════════════════════════════════╝

Type :help for help, :exit to quit
Supported languages: cpp, javascript
[INFO] BlockRegistry initialized: 4 blocks found

>>>
```

**Pass Criteria**: Banner displays with supported languages and block count

---

### Phase 3: REPL Commands

#### Test 2.1: :languages Command
```bash
./naab-repl
>>> :languages
```

**Expected Output**:
```
═══════════════════════════════════════════════════════════
  Supported Languages
═══════════════════════════════════════════════════════════

  • cpp          ✓ ready
  • javascript   ✓ ready
```

**Pass Criteria**: Lists cpp and javascript as ready

---

#### Test 2.2: :help Command
```bash
>>> :help
```

**Expected Output**:
```
═══════════════════════════════════════════════════════════
  NAAb REPL Commands
═══════════════════════════════════════════════════════════

General:
  :help, :h            Show this help message
  :exit, :quit, :q     Exit the REPL
  ...
```

**Pass Criteria**: Displays comprehensive help with all commands

---

#### Test 2.3: :blocks Command
```bash
>>> :blocks
```

**Expected Output**:
```
═══════════════════════════════════════════════════════════
  Available Blocks
═══════════════════════════════════════════════════════════

Total blocks: 4

  [cpp] (2 blocks)
    • BLOCK-CPP-MATH
    • BLOCK-CPP-VECTOR

  [javascript] (2 blocks)
    • BLOCK-JS-FORMAT
    • BLOCK-JS-STRING
```

**Pass Criteria**: Lists all 4 blocks grouped by language

---

### Phase 4: C++ Block Loading

#### Test 3.1: Load C++ Block in REPL
```bash
>>> :load BLOCK-CPP-MATH as math
>>> math.add(10, 20)
```

**Expected Output**:
```
[INFO] Loading block BLOCK-CPP-MATH as 'math'...
[INFO] Loaded block BLOCK-CPP-MATH from filesystem as math (cpp)
[INFO] Creating dedicated C++ executor for block...
[SUCCESS] Block BLOCK-CPP-MATH loaded and ready as 'math'

30
```

**Pass Criteria**: Block loads successfully and function returns correct result

---

#### Test 3.2: Run cpp_math.naab Program
```bash
./naab-lang run ../examples/cpp_math.naab
```

**Expected Output**:
```
=== C++ Math Block Demo ===

10 + 20 = 30
5 × 7 = 35
5² = 25.0
√16 = 4.0
max(15, 27) = 27

✓ C++ block executed successfully!
```

**Pass Criteria**: Program runs without errors and produces expected output

---

### Phase 5: JavaScript Block Loading

#### Test 4.1: Load JavaScript Block in REPL
```bash
>>> :load BLOCK-JS-STRING as str
>>> str.toUpper("hello world")
```

**Expected Output**:
```
[INFO] Loading block BLOCK-JS-STRING as 'str'...
[INFO] Loaded block BLOCK-JS-STRING from filesystem as str (javascript)
[INFO] Executing block with shared javascript executor...
[SUCCESS] Block BLOCK-JS-STRING loaded and ready as 'str'

"HELLO WORLD"
```

**Pass Criteria**: JavaScript block loads and string function works

---

#### Test 4.2: Run js_utils.naab Program
```bash
./naab-lang run ../examples/js_utils.naab
```

**Expected Output**:
```
=== JavaScript String Utils Demo ===

Uppercase: HELLO, WORLD!
Formatted: Hello, Alice! Welcome to NAAb.
Repeated 'Na' 3x: NaNaNa
Reversed 'stressed': desserts
Starts with 'NAAb': true

✓ JavaScript block executed successfully!
```

**Pass Criteria**: JavaScript block functions work correctly

---

### Phase 6: Polyglot Programs

#### Test 5.1: Run Polyglot Program
```bash
./naab-lang run ../examples/polyglot.naab
```

**Expected Output**:
```
=== Polyglot Program Demo ===
Combining C++ (speed) + JavaScript (formatting)

========================================
Statistics Report
========================================
Total: 55
Average: 5.5
Count: 10
========================================

✓ Multi-language program executed successfully!
✓ This is the power of NAAb Block Assembly!
```

**Pass Criteria**: Both C++ and JavaScript blocks work in same program

---

### Phase 7: Error Handling

#### Test 6.1: Unsupported Language
```bash
>>> use BLOCK-RUBY-001 as rb
```

**Expected Output**:
```
[ERROR] Block not found: BLOCK-RUBY-001
[ERROR] Checked BlockRegistry (4 blocks) and BlockLoader (unavailable)
```

**Pass Criteria**: Clear error message, no crash

---

#### Test 6.2: Block Not Found
```bash
>>> use BLOCK-NONEXISTENT as test
```

**Expected Output**:
```
[ERROR] Block not found: BLOCK-NONEXISTENT
[ERROR] Checked BlockRegistry (4 blocks) and BlockLoader (unavailable)
```

**Pass Criteria**: Graceful error, suggests available blocks

---

#### Test 6.3: Function Not Found
```bash
>>> :load BLOCK-CPP-MATH as math
>>> math.nonexistent_function(5)
```

**Expected Output**:
```
[ERROR] Function not found: nonexistent_function
```

**Pass Criteria**: Clear error message about missing function

---

### Phase 8: Performance Tests

#### Test 8.1: Multiple Block Loads
```bash
>>> :load BLOCK-CPP-MATH as math1
>>> :load BLOCK-CPP-MATH as math2
>>> :load BLOCK-CPP-VECTOR as vec
>>> :load BLOCK-JS-STRING as str
```

**Expected Behavior**: All blocks load without memory leaks or crashes

**Pass Criteria**:
- All 4 loads succeed
- Memory usage reasonable
- Response time < 500ms per block

---

#### Test 8.2: Session Persistence
```bash
>>> :load BLOCK-CPP-MATH as math
>>> let x = math.add(5, 10)
>>> print(x)
>>> let y = math.multiply(x, 2)
>>> print(y)
```

**Expected Output**:
```
15
30
```

**Pass Criteria**: Variables and blocks persist across statements

---

## Test Execution Checklist

### Setup
- [ ] Executables built successfully
- [ ] Example blocks copied to library directory
- [ ] BlockRegistry initialized with 4 blocks
- [ ] Both naab-lang and naab-repl executable

### Category 1: Executor Registration (4 tests)
- [ ] 7.1: Build naab-lang ✅
- [ ] 7.2: Build naab-repl ✅
- [ ] 1.1: Version command shows languages ✅
- [ ] 1.2: REPL banner shows languages ✅

### Category 2: REPL Commands (3 tests)
- [ ] 2.1: :languages command
- [ ] 2.2: :help command
- [ ] 2.3: :blocks command

### Category 3: C++ Block Loading (2 tests)
- [ ] 3.1: Load C++ block in REPL
- [ ] 3.2: Run cpp_math.naab

### Category 4: JS Block Loading (2 tests)
- [ ] 4.1: Load JS block in REPL
- [ ] 4.2: Run js_utils.naab

### Category 5: Polyglot Programs (1 test)
- [ ] 5.1: Run polyglot.naab

### Category 6: Error Handling (3 tests)
- [ ] 6.1: Unsupported language error
- [ ] 6.2: Block not found error
- [ ] 6.3: Function not found error

### Category 7: Performance (2 tests)
- [ ] 8.1: Multiple block loads
- [ ] 8.2: Session persistence

---

## Success Criteria

**Minimum Pass Rate**: 16/18 tests (89%)

**Required Passes**:
- All executor registration tests (4/4)
- All REPL command tests (3/3)
- At least 1 C++ block test (1/2)
- At least 1 JS block test (1/2)
- All error handling tests (3/3)

**Acceptable Failures**:
- Polyglot program (if blocks work individually)
- Performance edge cases

---

## Test Execution Strategy

### Step 1: Automated Setup (5 min)
```bash
# Verify build
cmake --build . --target naab-lang naab-repl

# Verify blocks
ls /storage/emulated/0/Download/.naab/naab/blocks/library/cpp/
ls /storage/emulated/0/Download/.naab/naab/blocks/library/javascript/

# Run block registry test
./test_block_registry
```

### Step 2: REPL Tests (10 min)
```bash
# Create test script
cat > test_repl.txt <<'EOF'
:languages
:help
:blocks
:load BLOCK-CPP-MATH as math
math.add(10, 20)
:load BLOCK-JS-STRING as str
str.toUpper("hello")
:exit
EOF

# Manual execution (REPL doesn't support stdin redirection yet)
# Run commands manually and record results
```

### Step 3: Example Programs (5 min)
```bash
# Copy executables to home
cp naab-lang /data/data/com.termux/files/home/

# Run examples
/data/data/com.termux/files/home/naab-lang parse ../examples/cpp_math.naab
/data/data/com.termux/files/home/naab-lang parse ../examples/js_utils.naab
/data/data/com.termux/files/home/naab-lang parse ../examples/polyglot.naab

# Note: Full execution requires C++ compilation support
```

### Step 4: Error Handling (5 min)
Test error cases manually in REPL

### Step 5: Documentation (10 min)
Create TEST_RESULTS.md with:
- Test outcomes (pass/fail)
- Actual vs expected output
- Screenshots or logs
- Issues discovered
- Pass rate calculation

---

## Expected Issues

### Known Limitations

1. **C++ Block Execution**:
   - Requires g++ compiler available
   - May need compilation flags configuration
   - Temporary .so file management

2. **Example Programs**:
   - May need `run` command implementation
   - Execution depends on block compilation

3. **REPL Input**:
   - No stdin redirection support yet
   - Manual testing required

---

## Deliverables

1. **TEST_RESULTS.md**: Complete test execution report
2. **Test logs**: Captured output from each test
3. **Bug reports**: Any issues discovered
4. **PHASE_9_COMPLETE.md**: Summary and recommendations

---

## Timeline

- **Setup**: 5 min
- **REPL tests**: 10 min
- **Example programs**: 5 min
- **Error handling**: 5 min
- **Documentation**: 10 min

**Total Estimated Time**: ~35 minutes

---

**Phase 9 Status**: ⏳ READY TO START

**Dependencies**:
- Phase 7 complete ✅
- Phase 8 complete ✅
- Blocks in library directory ✅
- Executables built ✅
