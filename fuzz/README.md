# Parser Fuzzing (libFuzzer)

Coverage-guided fuzz testing for the NAAb lexer and parser. Finds crashes, memory errors, and undefined behavior by feeding random/malformed `.naab` source through the tokenizer and parser.

## Build

```bash
cd build
cmake .. -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++
make fuzz_parser -j4
```

Requires Clang (libFuzzer is built-in). Automatically enables AddressSanitizer and UndefinedBehaviorSanitizer.

## Run

```bash
# Basic run with seed corpus
./fuzz_parser ../fuzz/corpus/ -max_len=10000 -timeout=10

# Run for N iterations then stop
./fuzz_parser ../fuzz/corpus/ -max_len=10000 -timeout=10 -runs=100000

# Run with more workers (parallel)
./fuzz_parser ../fuzz/corpus/ -max_len=10000 -timeout=10 -workers=4 -jobs=4

# If symbolizer hangs (common on Termux/ARM), disable it:
ASAN_SYMBOLIZER_PATH="" ASAN_OPTIONS=symbolize=0 \
    ./fuzz_parser ../fuzz/corpus/ -max_len=10000 -timeout=10 -runs=100000
```

## Corpus

The `corpus/` directory contains seed inputs covering major grammar features: functions, polyglot blocks, control flow, expressions, types, destructuring, imports, structs, and enums. The fuzzer mutates these to explore new code paths.

## What it catches

- Heap buffer overflows, use-after-free (ASAN)
- Signed integer overflow, null pointer dereference, UB (UBSAN)
- Infinite loops / hangs (timeout)
- Unhandled exceptions that bypass the catch-all (segfaults, aborts)

Normal parse errors (`ParseError`, `runtime_error`, `InputSizeException`, `RecursionLimitException`) are expected for malformed input and are silently caught.
