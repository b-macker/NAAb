# NAAb Fuzzing — requires Clang with libFuzzer
# Build: cmake .. -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++
# Run:   ./fuzz_parser ../fuzz/corpus/ -max_len=10000 -timeout=10
#         ./fuzz_interpreter ../fuzz/corpus_interpreter/ -max_len=10000 -timeout=10

if(NOT ENABLE_FUZZING)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzzing requires Clang (for built-in libFuzzer)")
endif()

# Add sanitizer + coverage instrumentation to all libraries under test
# so libFuzzer can guide mutations through code paths
foreach(target naab_lexer naab_parser naab_semantic
               naab_interpreter naab_runtime naab_stdlib naab_scanner)
    target_compile_options(${target} PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-omit-frame-pointer
        -g
    )
    target_link_options(${target} PRIVATE
        -fsanitize=address,undefined
    )
endforeach()

# Shared compile/link flags for fuzz targets
set(FUZZ_COMPILE_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
set(FUZZ_LINK_FLAGS -fsanitize=fuzzer,address,undefined)

# --- Fuzz target 1: lexer + parser only ---
add_executable(fuzz_parser fuzz_parser.cpp)
target_link_libraries(fuzz_parser
    naab_lexer
    naab_parser
    naab_semantic
    fmt::fmt
)
target_compile_options(fuzz_parser PRIVATE ${FUZZ_COMPILE_FLAGS})
target_link_options(fuzz_parser PRIVATE ${FUZZ_LINK_FLAGS})

# --- Fuzz target 2: full interpreter ---
add_executable(fuzz_interpreter fuzz_interpreter.cpp)
target_link_libraries(fuzz_interpreter
    naab_lexer
    naab_parser
    naab_semantic
    naab_interpreter
    naab_runtime
    naab_stdlib
    naab_scanner
    fmt::fmt
    spdlog::spdlog
)
target_compile_options(fuzz_interpreter PRIVATE ${FUZZ_COMPILE_FLAGS})
target_link_options(fuzz_interpreter PRIVATE ${FUZZ_LINK_FLAGS})

message(STATUS "  ✓ Fuzz target: fuzz_parser (libFuzzer + ASAN + UBSAN)")
message(STATUS "  ✓ Fuzz target: fuzz_interpreter (libFuzzer + ASAN + UBSAN)")
