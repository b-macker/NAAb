# NAAb Parser Fuzzing — requires Clang with libFuzzer
# Build: cmake .. -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++
# Run:   ./fuzz_parser ../fuzz/corpus/ -max_len=10000 -timeout=10

if(NOT ENABLE_FUZZING)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzzing requires Clang (for built-in libFuzzer)")
endif()

# Add sanitizer + coverage instrumentation to the libraries under test
# so libFuzzer can guide mutations through lexer/parser code paths
foreach(target naab_lexer naab_parser naab_semantic)
    target_compile_options(${target} PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined
        -fno-omit-frame-pointer
        -g
    )
    target_link_options(${target} PRIVATE
        -fsanitize=address,undefined
    )
endforeach()

# Fuzz target: lexer + parser only (no interpreter, no runtime)
add_executable(fuzz_parser fuzz_parser.cpp)

target_link_libraries(fuzz_parser
    naab_lexer
    naab_parser
    naab_semantic
    fmt::fmt
)

# libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer
target_compile_options(fuzz_parser PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -g
)
target_link_options(fuzz_parser PRIVATE
    -fsanitize=fuzzer,address,undefined
)

message(STATUS "  ✓ Fuzz target: fuzz_parser (libFuzzer + ASAN + UBSAN)")
