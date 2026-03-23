# NAAb Fuzz Testing (libFuzzer)

Coverage-guided fuzz testing with ASAN + UBSAN. Two targets:

1. **fuzz_parser** — random bytes through lexer → parser
2. **fuzz_interpreter** — valid-syntax adversarial programs through lexer → parser → interpreter

## Build

```bash
cd build
cmake .. -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++
make fuzz_parser fuzz_interpreter -j4
```

Requires Clang (libFuzzer is built-in). Automatically enables AddressSanitizer and UndefinedBehaviorSanitizer on all NAAb libraries.

## Run

```bash
# Parser fuzzer — random/malformed input
ASAN_SYMBOLIZER_PATH="" ASAN_OPTIONS=symbolize=0 \
    ./fuzz/fuzz_parser ../fuzz/corpus/ -max_len=10000 -timeout=10 -runs=100000

# Interpreter fuzzer — adversarial programs
ASAN_SYMBOLIZER_PATH="" ASAN_OPTIONS=symbolize=0 \
    ./fuzz/fuzz_interpreter ../fuzz/corpus_interpreter/ -max_len=10000 -timeout=10 -runs=500

# Run with more workers (parallel)
./fuzz/fuzz_parser ../fuzz/corpus/ -max_len=10000 -timeout=10 -workers=4 -jobs=4
```

Note: On Termux/ARM, the symbolizer may hang. Disable it with `ASAN_SYMBOLIZER_PATH="" ASAN_OPTIONS=symbolize=0`.

## Corpus

- `corpus/` — parser seeds: valid `.naab` covering all grammar features (functions, polyglot, control flow, etc.)
- `corpus_interpreter/` — interpreter seeds: adversarial programs stressing deep nesting, massive collections, recursive generators, pathological match, closure chains, GC pressure, exception storms, and struct nesting

## What it catches

- Heap buffer overflows, use-after-free (ASAN)
- Signed integer overflow, null pointer dereference, UB (UBSAN)
- Infinite loops / hangs (timeout)
- Unhandled exceptions that bypass the catch-all (segfaults, aborts)

Expected exceptions (`ParseError`, `runtime_error`, `NaabError`, `InputSizeException`, `RecursionLimitException`) are silently caught — only crashes and sanitizer violations are bugs.

## Interpreter fuzzer design

- Governance disabled (`disableGovernance()`) — no filesystem probing
- Python not initialized — polyglot blocks throw at runtime (caught)
- 50KB input cap — interpreter is slower per iteration than parser
- libFuzzer `-timeout=10` kills infinite loops (no built-in loop iteration limit)
