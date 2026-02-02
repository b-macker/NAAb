# NAAb Fuzzing Infrastructure

**Week 2, Task 2.1-2.2: Fuzzing Setup**

This directory contains libFuzzer-based fuzzing targets for discovering bugs, crashes, and security vulnerabilities in the NAAb language implementation.

## Fuzzing Targets

### Core Language Fuzzers

1. **fuzz_lexer** - Lexer/Tokenizer fuzzing
   - Tests: Token scanning, edge cases, malformed input
   - Corpus: `corpus/lexer/`

2. **fuzz_parser** - Parser fuzzing
   - Tests: Syntax parsing, recursion limits, malformed AST
   - Corpus: `corpus/parser/`

3. **fuzz_interpreter** - Interpreter fuzzing
   - Tests: Full execution pipeline, runtime errors, resource limits
   - Corpus: `corpus/interpreter/`

### FFI/Polyglot Boundary Fuzzers

4. **fuzz_python_executor** - Python FFI boundary fuzzing
   - Tests: Python execution, type marshaling, error handling
   - Corpus: `corpus/python/`

5. **fuzz_value_conversion** - Type conversion fuzzing
   - Tests: Value conversions, type marshaling, edge cases (NaN, infinity, overflow)
   - Corpus: `corpus/values/`

6. **fuzz_json_marshal** - JSON marshaling fuzzing
   - Tests: JSON parsing/serialization, deeply nested structures
   - Corpus: `corpus/json/`

## Building Fuzzers

Fuzzers require Clang compiler with libFuzzer support:

```bash
# Configure with Clang
export CC=clang
export CXX=clang++

# Build fuzzers
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=Debug
cmake --build build-fuzz -j4

# Fuzzers are in: build-fuzz/fuzz/
```

## Running Fuzzers

### Quick Test (1 minute each)

```bash
cd build-fuzz

# Lexer fuzzer
./fuzz/fuzz_lexer -max_total_time=60 ../fuzz/corpus/lexer/

# Parser fuzzer
./fuzz/fuzz_parser -max_total_time=60 ../fuzz/corpus/parser/

# Interpreter fuzzer
./fuzz/fuzz_interpreter -max_total_time=60 ../fuzz/corpus/interpreter/
```

### Continuous Fuzzing (Recommended)

For serious bug hunting, run for hours or days:

```bash
# Run for 1 hour with 4 parallel jobs
./fuzz/fuzz_parser \
    -max_total_time=3600 \
    -jobs=4 \
    -workers=4 \
    ../fuzz/corpus/parser/

# Run overnight (8 hours)
./fuzz/fuzz_interpreter \
    -max_total_time=28800 \
    -jobs=4 \
    -workers=4 \
    ../fuzz/corpus/interpreter/

# Run indefinitely in background
nohup ./fuzz/fuzz_parser \
    -jobs=4 \
    -workers=4 \
    ../fuzz/corpus/parser/ \
    > fuzzer.log 2>&1 &
```

### FFI Boundary Fuzzing

```bash
# Python executor fuzzing
./fuzz/fuzz_python_executor \
    -max_total_time=3600 \
    ../fuzz/corpus/python/

# Value conversion fuzzing
./fuzz/fuzz_value_conversion \
    -max_total_time=3600 \
    ../fuzz/corpus/values/

# JSON marshaling fuzzing
./fuzz/fuzz_json_marshal \
    -max_total_time=3600 \
    ../fuzz/corpus/json/
```

## Fuzzer Options

Common libFuzzer options:

```bash
# Time limits
-max_total_time=3600        # Run for 1 hour
-timeout=10                 # Kill hangs after 10 seconds

# Parallelization
-jobs=4                     # Run 4 fuzzing jobs in parallel
-workers=4                  # Use 4 worker processes

# Input limits
-max_len=10000             # Maximum input size

# Coverage tracking
-print_coverage=1          # Print coverage statistics
-print_pcs=1              # Print covered program counters

# Crash handling
-artifact_prefix=crash-    # Save crashes with this prefix
-exact_artifact_path=crash # Save exact crash reproducer

# Dictionary
-dict=my.dict             # Use fuzzing dictionary
```

## Understanding Results

### Success (No Crashes)

```
#1024   NEW    cov: 4567 ft: 12345 corp: 45/12KB exec/s: 123 rss: 64MB
```

- `cov`: Coverage (number of edges hit)
- `ft`: Features (unique execution paths)
- `corp`: Corpus size (number of interesting inputs)
- `exec/s`: Executions per second

### Crash Found

```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow
WRITE of size 8 at 0x602000001234
```

Crashes are saved to files like `crash-<hash>` or `leak-<hash>`.

To reproduce:

```bash
./fuzz/fuzz_parser crash-abc123
```

## Corpus Management

### Adding Seed Inputs

Add interesting test cases to corpus directories:

```bash
# Add new NAAb test case
echo "let x = 42" > corpus/lexer/new_test.naab

# Fuzzer will automatically discover it
./fuzz/fuzz_lexer corpus/lexer/
```

### Corpus Minimization

Reduce corpus size while maintaining coverage:

```bash
# Merge and minimize corpus
./fuzz/fuzz_parser \
    -merge=1 \
    corpus/parser/ \
    corpus/parser-minimized/

# Replace old corpus with minimized version
mv corpus/parser-minimized/* corpus/parser/
```

## Coverage Reports

Generate coverage reports with clang's source-based coverage:

```bash
# Build with coverage
cmake -B build-coverage \
    -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
cmake --build build-coverage

# Run fuzzers to generate profile data
LLVM_PROFILE_FILE="fuzzer.profraw" \
    ./build-coverage/fuzz/fuzz_parser corpus/parser/

# Generate coverage report
llvm-profdata merge -sparse fuzzer.profraw -o fuzzer.profdata
llvm-cov show ./build-coverage/fuzz/fuzz_parser \
    -instr-profile=fuzzer.profdata \
    -format=html \
    -output-dir=coverage_html/

# View report
open coverage_html/index.html
```

## Continuous Integration

Fuzzing in CI (GitHub Actions example):

```yaml
name: Fuzzing

on:
  schedule:
    - cron: '0 2 * * *'  # Run daily at 2 AM

jobs:
  fuzz:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - name: Build fuzzers
        run: |
          cmake -B build-fuzz
          cmake --build build-fuzz

      - name: Run fuzzers (1 hour each)
        run: |
          ./build-fuzz/fuzz/fuzz_lexer -max_total_time=3600 fuzz/corpus/lexer/
          ./build-fuzz/fuzz/fuzz_parser -max_total_time=3600 fuzz/corpus/parser/

      - name: Upload crashes
        if: failure()
        uses: actions/upload-artifact@v3
        with:
          name: fuzzer-crashes
          path: crash-*
```

## OSS-Fuzz Integration (Optional)

To integrate with Google's OSS-Fuzz for continuous 24/7 fuzzing:

1. Fork repository to https://github.com/google/oss-fuzz
2. Add `projects/naab/` directory with:
   - `project.yaml` - Project configuration
   - `Dockerfile` - Build environment
   - `build.sh` - Fuzzer build script

See `docs/OSS_FUZZ.md` for detailed instructions (to be created in Task 2.3).

## Troubleshooting

### Fuzzer Hangs

If fuzzer seems stuck:
- Check timeout settings (`-timeout=10`)
- Reduce max input size (`-max_len=10000`)
- Check for infinite loops in code under test

### Low Coverage

If coverage isn't increasing:
- Add more seed inputs to corpus
- Use fuzzing dictionaries (`-dict=`)
- Check that sanitizers are enabled (they add coverage feedback)

### Out of Memory

If fuzzer crashes with OOM:
- Reduce input size (`-max_len=`)
- Reduce parallelization (`-jobs=1`)
- Check for memory leaks (sanitizers will report)

## Security Impact

Fuzzing discovers:
- **Buffer overflows** - Memory safety violations
- **Use-after-free** - Dangling pointer bugs
- **Integer overflows** - Arithmetic bugs
- **Assertion failures** - Logic errors
- **Infinite loops** - Hang/DoS vulnerabilities
- **Memory leaks** - Resource exhaustion

Combined with sanitizers (ASan, UBSan), fuzzing provides comprehensive bug detection.

## Weekly Fuzzing Schedule

**Recommended**: Run fuzzers continuously on a dedicated machine or CI:

- **Daily**: 1-hour quick fuzzing of all targets
- **Weekly**: 24-hour deep fuzzing campaign
- **Before release**: 72-hour comprehensive fuzzing

Any crashes discovered should be:
1. Reproduced locally
2. Fixed immediately
3. Added to test suite
4. Corpus updated with reproducer

## Results and Metrics

Track fuzzing effectiveness:

```bash
# Get corpus stats
ls -lh corpus/parser/ | wc -l    # Number of inputs
du -sh corpus/parser/             # Corpus size

# Get coverage stats
./fuzz/fuzz_parser -runs=0 corpus/parser/  # Dry run for stats
```

Aim for:
- **Coverage**: >80% code coverage
- **Corpus**: >100 diverse inputs per target
- **Crashes**: 0 crashes after 24-hour campaign

## Next Steps: Week 3

After fuzzing setup is complete, Week 3 will focus on **Supply Chain Security**:
- Dependency pinning and lockfiles
- SBOM generation
- Artifact signing
- Secret scanning

---

**Last Updated**: 2026-01-30 (Week 2, Day 1)
**Status**: Fuzzing infrastructure complete, ready for continuous fuzzing
