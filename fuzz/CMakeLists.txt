# NAAb Fuzzing Infrastructure
# Week 2, Task 2.1: Fuzzing Setup

# Only build fuzzers with Clang (libFuzzer requirement)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "Building fuzzing targets with Clang...")

    # Common fuzzer compile and link options
    set(FUZZER_FLAGS -fsanitize=fuzzer,address,undefined -g)

    # ========================================================================
    # Lexer Fuzzer
    # ========================================================================
    add_executable(fuzz_lexer fuzz_lexer.cpp)
    target_link_libraries(fuzz_lexer
        naab_lexer
        absl::strings
        absl::flat_hash_map
    )
    target_compile_options(fuzz_lexer PRIVATE ${FUZZER_FLAGS})
    target_link_options(fuzz_lexer PRIVATE ${FUZZER_FLAGS})
    target_include_directories(fuzz_lexer PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    message(STATUS "  ✓ Lexer fuzzer")

    # ========================================================================
    # Parser Fuzzer
    # ========================================================================
    add_executable(fuzz_parser fuzz_parser.cpp)
    target_link_libraries(fuzz_parser
        naab_parser
        naab_lexer
        naab_semantic
        absl::strings
        absl::flat_hash_map
        absl::statusor
        fmt::fmt
    )
    target_compile_options(fuzz_parser PRIVATE ${FUZZER_FLAGS})
    target_link_options(fuzz_parser PRIVATE ${FUZZER_FLAGS})
    target_include_directories(fuzz_parser PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
    )
    message(STATUS "  ✓ Parser fuzzer")

    # ========================================================================
    # Interpreter Fuzzer
    # ========================================================================
    add_executable(fuzz_interpreter fuzz_interpreter.cpp)
    target_link_libraries(fuzz_interpreter
        naab_interpreter
        naab_parser
        naab_lexer
        naab_runtime
        naab_stdlib
        naab_semantic
        naab_security
        absl::strings
        absl::flat_hash_map
        fmt::fmt
        spdlog::spdlog
        SQLite::SQLite3
    )
    if(Python3_FOUND)
        target_link_libraries(fuzz_interpreter ${Python3_LIBRARIES})
    endif()
    target_compile_options(fuzz_interpreter PRIVATE ${FUZZER_FLAGS})
    target_link_options(fuzz_interpreter PRIVATE ${FUZZER_FLAGS})
    target_include_directories(fuzz_interpreter PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
    )
    message(STATUS "  ✓ Interpreter fuzzer")

    # ========================================================================
    # Python Executor Fuzzer (Week 2, Task 2.2)
    # ========================================================================
    if(HAVE_PYBIND11)
        add_executable(fuzz_python_executor fuzz_python_executor.cpp)
        target_link_libraries(fuzz_python_executor
            naab_runtime
            naab_interpreter
            naab_security
            ${Python3_LIBRARIES}
            fmt::fmt
        )
        target_compile_options(fuzz_python_executor PRIVATE ${FUZZER_FLAGS})
        target_link_options(fuzz_python_executor PRIVATE ${FUZZER_FLAGS})
        target_include_directories(fuzz_python_executor PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_BINARY_DIR}/include
            ${Python3_INCLUDE_DIRS}
        )
        message(STATUS "  ✓ Python executor fuzzer")
    else()
        message(STATUS "  ⚠ Python executor fuzzer skipped (pybind11 not found)")
    endif()

    # ========================================================================
    # Value Conversion Fuzzer (Week 2, Task 2.2)
    # ========================================================================
    add_executable(fuzz_value_conversion fuzz_value_conversion.cpp)
    target_link_libraries(fuzz_value_conversion
        naab_interpreter
        naab_runtime
        naab_stdlib
        fmt::fmt
    )
    target_compile_options(fuzz_value_conversion PRIVATE ${FUZZER_FLAGS})
    target_link_options(fuzz_value_conversion PRIVATE ${FUZZER_FLAGS})
    target_include_directories(fuzz_value_conversion PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
    )
    message(STATUS "  ✓ Value conversion fuzzer")

    # ========================================================================
    # JSON Marshal Fuzzer (Week 2, Task 2.2)
    # ========================================================================
    add_executable(fuzz_json_marshal fuzz_json_marshal.cpp)
    target_link_libraries(fuzz_json_marshal
        naab_interpreter
        naab_parser
        naab_lexer
        naab_runtime
        naab_stdlib
        naab_semantic
        fmt::fmt
    )
    target_compile_options(fuzz_json_marshal PRIVATE ${FUZZER_FLAGS})
    target_link_options(fuzz_json_marshal PRIVATE ${FUZZER_FLAGS})
    target_include_directories(fuzz_json_marshal PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
    )
    message(STATUS "  ✓ JSON marshal fuzzer")

    message(STATUS "")
    message(STATUS "Fuzzing targets built successfully!")
    message(STATUS "Run fuzzers:")
    message(STATUS "  ./fuzz/fuzz_lexer corpus/lexer/")
    message(STATUS "  ./fuzz/fuzz_parser corpus/parser/")
    message(STATUS "  ./fuzz/fuzz_interpreter corpus/interpreter/")
    message(STATUS "  ./fuzz/fuzz_python_executor corpus/python/")
    message(STATUS "  ./fuzz/fuzz_value_conversion corpus/values/")
    message(STATUS "  ./fuzz/fuzz_json_marshal corpus/json/")

else()
    message(STATUS "Fuzzing targets require Clang compiler (skipped)")
endif()
