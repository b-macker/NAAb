cmake_minimum_required(VERSION 3.15)

# Project with semantic versioning
project(naab_lang
    VERSION 0.1.0
    DESCRIPTION "NAAb Block Assembly Language"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

# ============================================================================
# Sanitizer Build Options (Week 1: Security Hardening Sprint)
# ============================================================================
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_MSAN "Enable MemorySanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# AddressSanitizer (memory errors: use-after-free, buffer overflow, leaks)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
    message(STATUS "🔍 AddressSanitizer enabled")
endif()

# UndefinedBehaviorSanitizer (undefined behavior: null deref, integer overflow, etc.)
if(ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
    message(STATUS "🔍 UndefinedBehaviorSanitizer enabled")
endif()

# MemorySanitizer (uninitialized reads)
if(ENABLE_MSAN)
    add_compile_options(-fsanitize=memory -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=memory)
    message(STATUS "🔍 MemorySanitizer enabled")
endif()

# ThreadSanitizer (data races)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
    message(STATUS "🔍 ThreadSanitizer enabled")
endif()

# ============================================================================
# Advanced Security Options (Phase 1: Path to 97%)
# ============================================================================
option(ENABLE_CFI "Enable Control Flow Integrity" OFF)
option(ENABLE_HARDENING "Enable additional hardening flags" ON)

# Control Flow Integrity (CFI) - prevents control flow hijacking
if(ENABLE_CFI)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fsanitize=cfi -flto -fvisibility=hidden)
        add_link_options(-fsanitize=cfi -flto)
        message(STATUS "🛡️  Control Flow Integrity (CFI) enabled")
    else()
        message(WARNING "CFI requires Clang compiler")
    endif()
endif()

# Hardening flags (enabled by default)
if(ENABLE_HARDENING)
    # Stack protection
    add_compile_options(-fstack-protector-strong)

    # Position-independent code (PIE) for ASLR
    add_compile_options(-fPIE)
    add_link_options(-pie)

    # Fortify source (buffer overflow detection)
    add_compile_definitions(_FORTIFY_SOURCE=2)

    # Integer conversion warnings
    add_compile_options(
        -Wconversion
        -Wsign-conversion
        -Wnarrowing
    )

    # Additional security warnings
    add_compile_options(
        -Wformat=2
        -Wformat-security
        -Werror=format-security
    )

    message(STATUS "🛡️  Hardening flags enabled (stack protection, PIE, fortify, conversion warnings)")
endif()

# Extract git information for build metadata
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Provide defaults if git is not available
if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
endif()

if(NOT GIT_DESCRIBE)
    set(GIT_DESCRIBE "v${PROJECT_VERSION}")
endif()

# Build timestamp (UTC)
string(TIMESTAMP BUILD_TIMESTAMP "%Y%m%d.%H%M%S" UTC)

# Version variables
set(NAAB_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(NAAB_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(NAAB_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(NAAB_VERSION_STRING "${PROJECT_VERSION}")
set(NAAB_GIT_HASH "${GIT_COMMIT_HASH}")
set(NAAB_BUILD_TIMESTAMP "${BUILD_TIMESTAMP}")
set(NAAB_VERSION_FULL "${PROJECT_VERSION}+${BUILD_TIMESTAMP}.${GIT_COMMIT_HASH}")

message(STATUS "======================================")
message(STATUS "NAAb Block Assembly Language")
message(STATUS "Version: ${NAAB_VERSION_STRING}")
message(STATUS "Git: ${NAAB_GIT_HASH}")
message(STATUS "Build: ${NAAB_BUILD_TIMESTAMP}")
message(STATUS "======================================")

# Add external libraries from blocks
message(STATUS "Adding external dependencies...")

# Abseil (from blocks)
add_subdirectory(external/abseil-cpp EXCLUDE_FROM_ALL)
message(STATUS "  ✓ Abseil C++ library")

# fmt (from blocks)
add_subdirectory(external/fmt EXCLUDE_FROM_ALL)
message(STATUS "  ✓ fmt formatting library")

# spdlog (from blocks)
add_subdirectory(external/spdlog EXCLUDE_FROM_ALL)
message(STATUS "  ✓ spdlog logging library")

# toml++ (for formatter configuration)
add_subdirectory(external/tomlplusplus EXCLUDE_FROM_ALL)
message(STATUS "  ✓ toml++ TOML parser")

# SQLite3 for block registry
find_package(SQLite3 REQUIRED)
if(SQLite3_FOUND)
    message(STATUS "  ✓ SQLite3 (block registry support)")
else()
    message(FATAL_ERROR "SQLite3 required for block loading")
endif()

# Python3 for Python block execution
find_package(Python3 COMPONENTS Interpreter Development QUIET)
if(Python3_FOUND)
    message(STATUS "  ✓ Python ${Python3_VERSION} (Python block execution)")
    include_directories(${Python3_INCLUDE_DIRS})

    # Check for pybind11
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -m pybind11 --includes
        OUTPUT_VARIABLE PYBIND11_INCLUDES
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE PYBIND11_RESULT
    )
    if(PYBIND11_RESULT EQUAL 0)
        message(STATUS "  ✓ pybind11 (C++/Python binding)")
        # Parse include flags
        string(REPLACE " " ";" PYBIND11_INCLUDE_LIST ${PYBIND11_INCLUDES})
        foreach(INCLUDE_FLAG ${PYBIND11_INCLUDE_LIST})
            string(REPLACE "-I" "" INCLUDE_DIR ${INCLUDE_FLAG})
            include_directories(${INCLUDE_DIR})
        endforeach()
        set(HAVE_PYBIND11 TRUE)
    else()
        message(STATUS "  ⚠ pybind11 not found (Python executor disabled)")
        set(HAVE_PYBIND11 FALSE)
    endif()
else()
    message(STATUS "  ⚠ Python3 not found (Python blocks disabled)")
    set(HAVE_PYBIND11 FALSE)
endif()

# OpenSSL for crypto functions
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    message(STATUS "  ✓ OpenSSL ${OPENSSL_VERSION} (crypto stdlib functions)")
else()
    message(STATUS "  ⚠ OpenSSL not found (crypto hash functions disabled)")
endif()

# libffi for dynamic C++ function calling
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFI QUIET libffi)
    if(FFI_FOUND)
        message(STATUS "  ✓ libffi ${FFI_VERSION} (dynamic C++ function calling)")
    else()
        message(STATUS "  ⚠ libffi not found (C++ executor limited to hardcoded signatures)")
    endif()
else()
    message(STATUS "  ⚠ pkg-config not found (cannot detect libffi)")
endif()

# Rust for Rust block execution (Phase 3.1-3.3)
# Enable Rust support unconditionally (FFI implemented, executor ready)
set(HAVE_RUST TRUE)
message(STATUS "  ✓ Rust FFI implemented (Rust block execution enabled)")

# Include directories
include_directories(include)
include_directories(external/abseil-cpp)
include_directories(external/fmt/include)
include_directories(external/spdlog/include)
include_directories(external/cpp-httplib)

# Configuration for block paths
set(NAAB_BLOCKS_PATH "/storage/emulated/0/Download/.naab/naab/blocks/library")
set(NAAB_DATABASE_PATH "/storage/emulated/0/Download/.naab/naab/data/naab.db")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/naab/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/naab/config.h"
)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/include")

message(STATUS "Building NAAb interpreter components...")

# Lexer library
add_library(naab_lexer
    src/lexer/lexer.cpp
)
target_link_libraries(naab_lexer
    absl::strings
    absl::flat_hash_map
)
message(STATUS "  ✓ Lexer library")

# Parser library (assemble from LLVM blocks)
add_library(naab_parser
    src/parser/parser.cpp
    src/parser/ast_nodes.cpp
    src/parser/error_hints.cpp
)
target_link_libraries(naab_parser
    naab_lexer
    absl::flat_hash_map
    absl::statusor
)
message(STATUS "  ✓ Parser library (from LLVM blocks)")
message(STATUS "  ✓ Parser error hints (Phase 4.2)")

# Semantic analyzer (use Clang type checker blocks)
add_library(naab_semantic
    src/semantic/analyzer.cpp
    src/semantic/symbol_table.cpp
    src/semantic/type_checker.cpp
    src/semantic/type_system.cpp  # Type system implementation
    src/semantic/error_reporter.cpp
    src/semantic/error_helpers.cpp
    src/semantic/suggestion_system.cpp  # Phase 4.1: Enhanced error suggestions
    src/semantic/composition_validator.cpp  # Phase 4.3: Validation command
)
target_link_libraries(naab_semantic
    naab_parser
    absl::flat_hash_map
    fmt::fmt
)
message(STATUS "  ✓ Semantic analyzer (from Clang blocks)")
message(STATUS "  ✓ Type checker")
message(STATUS "  ✓ Error reporter")
message(STATUS "  ✓ Error helpers (fuzzy matching)")

# Formatter library (Phase 4.2: Auto-formatter)
add_library(naab_formatter
    src/formatter/formatter.cpp
    src/formatter/style_config.cpp
)
target_link_libraries(naab_formatter
    naab_parser
    naab_lexer
    fmt::fmt
    tomlplusplus::tomlplusplus
)
message(STATUS "  ✓ Formatter library (auto-formatter)")

# Linter library (Phase 4.2: Code quality hints)
add_library(naab_linter
    src/linter/llm_patterns.cpp
    src/linter/quality_hints.cpp
)
target_link_libraries(naab_linter
    naab_parser
    fmt::fmt
)
message(STATUS "  ✓ Linter library (quality hints)")

# Debugger library
add_library(naab_debugger
    src/debugger/debugger.cpp
)
target_link_libraries(naab_debugger
    absl::flat_hash_map
    fmt::fmt
)
message(STATUS "  ✓ Debugger")

# Security library (Phase 1: Security Hardening + Phase 3: Sandboxing)
add_library(naab_security
    src/runtime/resource_limits.cpp
    src/runtime/input_validator.cpp
    src/runtime/crypto_utils.cpp
    src/runtime/audit_logger.cpp
    src/runtime/sandbox.cpp
    src/safe_regex.cpp                       # Phase 1 Item 7: Regex timeout protection
    src/runtime/tamper_evident_logger.cpp    # Phase 1 Item 8: Tamper-evident logging
    src/runtime/ffi_callback_validator.cpp   # Phase 1 Item 9: FFI callback safety
    src/runtime/ffi_async_callback.cpp       # Phase 1 Item 10: FFI async callback safety
)
target_link_libraries(naab_security
    fmt::fmt
)
# Link OpenSSL if available
if(OPENSSL_FOUND)
    target_link_libraries(naab_security OpenSSL::SSL OpenSSL::Crypto)
endif()
message(STATUS "  ✓ Security (timeouts, validation, integrity, audit, sandbox)")

# Interpreter
add_library(naab_interpreter
    src/interpreter/interpreter.cpp
    src/interpreter/evaluator.cpp
    src/interpreter/environment.cpp
    src/interpreter/cycle_detector.cpp  # Phase 3.2: Garbage collection
    src/interpreter/error_context.cpp   # Phase 4.2: Enhanced error reporting
    src/interpreter/debugger.cpp        # Phase 4.2: Interactive debugger
    src/interpreter/polyglot_dependency_analyzer.cpp  # Parallel polyglot execution
)
target_link_libraries(naab_interpreter
    naab_parser
    naab_semantic
    naab_stdlib
    naab_debugger
    absl::flat_hash_map
    fmt::fmt
)
if(Python3_FOUND)
    target_link_libraries(naab_interpreter ${Python3_LIBRARIES})
endif()
message(STATUS "  ✓ Interpreter")
message(STATUS "  ✓ Runtime error enhancement (Phase 4.2)")

# QuickJS library for JavaScript execution
add_library(quickjs STATIC IMPORTED)
set_target_properties(quickjs PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/external/quickjs-2021-03-27/libquickjs.a"
)
target_include_directories(quickjs INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/external/quickjs-2021-03-27"
)
message(STATUS "  ✓ QuickJS library")

# Runtime + block loader
set(NAAB_RUNTIME_SOURCES
    src/runtime/block_loader.cpp
    src/runtime/block_registry.cpp
    src/runtime/block_search_index.cpp
    src/runtime/cpp_executor.cpp
    src/runtime/type_marshaller.cpp
    src/runtime/semver.cpp
    src/runtime/js_executor.cpp
    src/runtime/language_registry.cpp
    src/runtime/cpp_executor_adapter.cpp
    src/runtime/js_executor_adapter.cpp
    src/runtime/shell_executor.cpp  # Polyglot: Shell executor
    src/runtime/generic_subprocess_executor.cpp  # Polyglot: Generic subprocess executor
    src/runtime/subprocess_helpers.cpp  # Polyglot: Subprocess utility functions
    src/runtime/cross_language_bridge.cpp
    src/runtime/module_resolver.cpp  # Phase 3.1: Module system
    src/runtime/module_system.cpp    # Phase 4.0: Module system (Rust-style)
    src/runtime/struct_registry.cpp  # Struct support
    src/runtime/cpp_block_interface.cpp  # C FFI for struct support
    src/runtime/rust_ffi_bridge.cpp  # Phase 3.1: Rust FFI bridge
    src/runtime/rust_executor.cpp  # Phase 3.1: Rust executor
    src/runtime/csharp_executor.cpp  # Phase 11: C# executor
    src/runtime/stack_tracer.cpp  # Phase 4.2: Cross-language stack traces
    src/runtime/stack_formatter.cpp  # Phase 4.2.6: Enhanced stack trace formatting
    src/runtime/inline_code_cache.cpp  # Phase 3.3.1: Inline code caching
    src/runtime/polyglot_async_executor.cpp  # Phase 1 Item 10 Day 5: Polyglot async execution
    src/runtime/logger.cpp  # Logging infrastructure
)

# Add Python executor if pybind11 is available
if(HAVE_PYBIND11)
    list(APPEND NAAB_RUNTIME_SOURCES src/runtime/python_interpreter_manager.cpp)  # Phase 2: Global Python interpreter
    list(APPEND NAAB_RUNTIME_SOURCES src/runtime/python_executor.cpp)
    list(APPEND NAAB_RUNTIME_SOURCES src/runtime/python_executor_adapter.cpp)
endif()

add_library(naab_runtime ${NAAB_RUNTIME_SOURCES})
target_link_libraries(naab_runtime
    absl::flat_hash_map
    absl::statusor
    fmt::fmt
    SQLite::SQLite3
    quickjs
    naab_block_enricher  # For library detection
    naab_security        # Phase 1: Security hardening
    dl  # For dlopen/dlsym
    pthread  # Required by QuickJS
    m  # Math library required by QuickJS
)

# Add libffi if available
if(FFI_FOUND)
    target_include_directories(naab_runtime PRIVATE ${FFI_INCLUDE_DIRS})
    target_link_libraries(naab_runtime ${FFI_LIBRARIES})
    target_compile_definitions(naab_runtime PRIVATE HAVE_LIBFFI=1)
endif()

# Add Python libraries if pybind11 is available
if(HAVE_PYBIND11)
    target_link_libraries(naab_runtime ${Python3_LIBRARIES})
    target_compile_definitions(naab_runtime PRIVATE HAVE_PYBIND11=1)
endif()

# Add Rust support (Phase 3.1-3.3)
if(HAVE_RUST)
    target_compile_definitions(naab_runtime PRIVATE HAVE_RUST=1)
endif()

target_include_directories(naab_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include
)
message(STATUS "  ✓ Runtime & block loader (SQLite3)")

# Manifest system
add_library(naab_manifest
    src/manifest/manifest_loader.cpp
)
target_link_libraries(naab_manifest
    fmt::fmt
    tomlplusplus::tomlplusplus
)
message(STATUS "  ✓ Manifest system")

# Standard library
add_library(naab_stdlib
    src/stdlib/core.cpp
    src/stdlib/io.cpp
    src/stdlib/collections.cpp
    src/stdlib/json_impl.cpp
    src/stdlib/http_impl.cpp
    src/stdlib/stdlib.cpp
    # New stdlib modules
    src/stdlib/string_impl.cpp
    src/stdlib/array_impl.cpp
    src/stdlib/math_impl.cpp
    src/stdlib/time_impl.cpp
    src/stdlib/env_impl.cpp
    src/stdlib/csv_impl.cpp
    src/stdlib/regex_impl.cpp
    src/stdlib/crypto_impl.cpp
    src/stdlib/file_impl.cpp
    # Utility modules
    src/utils/error_formatter.cpp
    src/utils/string_utils.cpp
)
target_include_directories(naab_stdlib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/json/include
)
target_link_libraries(naab_stdlib
    fmt::fmt
    spdlog::spdlog
    absl::strings
    curl
    naab_security  # For SafeRegex ReDoS protection
)
if(OPENSSL_FOUND)
    target_link_libraries(naab_stdlib OpenSSL::SSL OpenSSL::Crypto)
endif()
message(STATUS "  ✓ Standard library")

# REST API (Phase 4.7)
add_library(naab_rest_api
    src/api/rest_api.cpp
)
target_include_directories(naab_rest_api PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/cpp-httplib
)
target_link_libraries(naab_rest_api
    naab_interpreter
    naab_runtime
    fmt::fmt
    spdlog::spdlog
    pthread
)
message(STATUS "  ✓ REST API library (Phase 4.7)")

# Testing framework
add_library(naab_testing
    src/testing/block_tester.cpp
)
target_link_libraries(naab_testing
    naab_runtime
    naab_interpreter
    fmt::fmt
)
message(STATUS "  ✓ Testing framework")

# Profiling framework
add_library(naab_profiling
    src/profiling/profiler.cpp
)
target_link_libraries(naab_profiling
    fmt::fmt
)
message(STATUS "  ✓ Profiling framework")

# Main executable
add_executable(naab-lang
    src/cli/main.cpp
)
target_link_libraries(naab-lang
    naab_lexer
    naab_parser
    naab_semantic
    naab_formatter
    naab_linter
    naab_interpreter
    naab_runtime
    naab_stdlib
    naab_rest_api
    naab_manifest
    fmt::fmt
    spdlog::spdlog
)
if(HAVE_PYBIND11)
    target_compile_definitions(naab-lang PRIVATE HAVE_PYBIND11=1)
endif()
if(HAVE_RUST)
    target_compile_definitions(naab-lang PRIVATE HAVE_RUST=1)
endif()
message(STATUS "  ✓ Main executable (naab-lang)")

# REPL executable
add_executable(naab-repl
    src/repl/repl.cpp
    src/repl/repl_commands.cpp
)
target_link_libraries(naab-repl
    naab_interpreter
    naab_runtime
    naab_stdlib
    fmt::fmt
    spdlog::spdlog
)
message(STATUS "  ✓ REPL executable (naab-repl)")

# Optimized REPL executable (O(1) incremental execution)
add_executable(naab-repl-opt
    src/repl/repl_optimized.cpp
)
target_link_libraries(naab-repl-opt
    naab_interpreter
    naab_runtime
    naab_stdlib
    fmt::fmt
    spdlog::spdlog
)
message(STATUS "  ✓ Optimized REPL executable (naab-repl-opt)")

# Linenoise library for readline support
add_library(linenoise STATIC
    external/linenoise.c
)
set_target_properties(linenoise PROPERTIES LINKER_LANGUAGE C)
target_include_directories(linenoise PUBLIC external)
message(STATUS "  ✓ Linenoise library")

# REPL with readline support (linenoise)
add_executable(naab-repl-rl
    src/repl/repl_readline.cpp
)
target_link_libraries(naab-repl-rl
    naab_interpreter
    naab_runtime
    naab_stdlib
    linenoise
    fmt::fmt
    spdlog::spdlog
)
message(STATUS "  ✓ Readline REPL executable (naab-repl-rl)")

# Documentation generator library
add_library(naab_doc_generator
    src/doc/doc_generator.cpp
)
target_link_libraries(naab_doc_generator
    fmt::fmt
)
message(STATUS "  ✓ Documentation generator library")

# Documentation generator CLI tool
add_executable(naab-doc
    src/cli/doc_main.cpp
)
target_link_libraries(naab-doc
    naab_doc_generator
    fmt::fmt
)
message(STATUS "  ✓ Documentation generator tool (naab-doc)")

# Phase 1 Item 8: Tamper-evident log verification tool
add_executable(naab-verify-audit
    src/cli/verify_audit.cpp
)
target_link_libraries(naab-verify-audit
    naab_security
    fmt::fmt
)
if(OPENSSL_FOUND)
    target_link_libraries(naab-verify-audit OpenSSL::SSL OpenSSL::Crypto)
endif()
message(STATUS "  ✓ Tamper-evident log verification tool (naab-verify-audit)")

# Phase 4.1: LSP Server
add_executable(naab-lsp
    tools/naab-lsp/main.cpp
    tools/naab-lsp/lsp_server.cpp
    tools/naab-lsp/json_rpc.cpp
    tools/naab-lsp/document_manager.cpp
    tools/naab-lsp/symbol_provider.cpp
    tools/naab-lsp/hover_provider.cpp
    tools/naab-lsp/completion_provider.cpp
    tools/naab-lsp/definition_provider.cpp
)

target_include_directories(naab-lsp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external/json/single_include
)

target_link_libraries(naab-lsp PRIVATE
    naab_parser
    naab_semantic
    naab_interpreter
    fmt::fmt
)

install(TARGETS naab-lsp DESTINATION bin)
message(STATUS "  ✓ LSP Server (naab-lsp)")

# Block Enricher library
add_library(naab_block_enricher
    tools/block_enricher.cpp
)
target_link_libraries(naab_block_enricher
    fmt::fmt
)
target_include_directories(naab_block_enricher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include
)
message(STATUS "  ✓ Block enricher library")

# Block Enrichment tool
add_executable(enrich_tool
    tools/enrich_tool.cpp
)
target_link_libraries(enrich_tool
    naab_block_enricher
    fmt::fmt
)
target_include_directories(enrich_tool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include
)
message(STATUS "  ✓ Block enrichment tool (enrich_tool)")

# C++ Executor Test
add_executable(test_cpp_executor
    tests/unit/test_cpp_executor.cpp
)
target_link_libraries(test_cpp_executor
    naab_runtime
    naab_interpreter
    naab_parser
    naab_semantic
    naab_stdlib
    fmt::fmt
    spdlog::spdlog
)
if(Python3_FOUND)
    target_link_libraries(test_cpp_executor ${Python3_LIBRARIES})
    target_include_directories(test_cpp_executor PRIVATE ${Python3_INCLUDE_DIRS})
endif()
message(STATUS "  ✓ C++ executor test")

# JavaScript Executor Test
add_executable(test_js_executor
    tests/unit/test_js_executor.cpp
)
target_link_libraries(test_js_executor
    naab_runtime
    naab_interpreter
    naab_parser
    naab_semantic
    naab_stdlib
    fmt::fmt
    spdlog::spdlog
)
if(Python3_FOUND)
    target_link_libraries(test_js_executor ${Python3_LIBRARIES})
    target_include_directories(test_js_executor PRIVATE ${Python3_INCLUDE_DIRS})
endif()
message(STATUS "  ✓ JavaScript executor test")

# Language Registry Test
add_executable(test_language_registry
    tests/unit/test_language_registry.cpp
)
target_link_libraries(test_language_registry
    naab_runtime
    naab_interpreter
    naab_parser
    naab_semantic
    naab_stdlib
    fmt::fmt
    spdlog::spdlog
)
if(Python3_FOUND)
    target_link_libraries(test_language_registry ${Python3_LIBRARIES})
    target_include_directories(test_language_registry PRIVATE ${Python3_INCLUDE_DIRS})
endif()
message(STATUS "  ✓ Language registry test")

# Block Tester Test
add_executable(test_block_tester
    tests/unit/test_block_tester.cpp
)
target_link_libraries(test_block_tester
    naab_testing
    naab_runtime
    naab_interpreter
    naab_parser
    naab_semantic
    naab_stdlib
    fmt::fmt
    spdlog::spdlog
)
if(Python3_FOUND)
    target_link_libraries(test_block_tester ${Python3_LIBRARIES})
    target_include_directories(test_block_tester PRIVATE ${Python3_INCLUDE_DIRS})
endif()
message(STATUS "  ✓ Block tester test")

# Profiler Test
add_executable(test_profiler
    tests/unit/test_profiler.cpp
)
target_link_libraries(test_profiler
    naab_profiling
    fmt::fmt
)
message(STATUS "  ✓ Profiler test")

# Block Registry Test
add_executable(test_block_registry
    tests/unit/test_block_registry.cpp
)
target_link_libraries(test_block_registry
    naab_runtime
    fmt::fmt
)
message(STATUS "  ✓ Block registry test")

# Enhanced Metadata Test (Phase 1.4)
add_executable(test_enhanced_metadata
    tests/unit/test_enhanced_metadata.cpp
)
target_link_libraries(test_enhanced_metadata
    naab_runtime
    fmt::fmt
)
message(STATUS "  ✓ Enhanced metadata test (Phase 1.4)")

# Search Index Test (Phase 1.5)
add_executable(test_search_index
    tests/unit/test_search_index.cpp
)
target_link_libraries(test_search_index
    naab_runtime
    fmt::fmt
    SQLite::SQLite3
)
message(STATUS "  ✓ Search index test (Phase 1.5)")

# Error Reporting Test (Phase 1.3)
add_executable(test_error_reporting
    tests/unit/test_error_reporting.cpp
)
target_link_libraries(test_error_reporting
    naab_lexer
    naab_parser
    naab_semantic
    fmt::fmt
)
message(STATUS "  ✓ Error reporting test (Phase 1.3)")

# Example Programs Parse Test
add_executable(test_examples
    tests/unit/test_examples.cpp
)
target_link_libraries(test_examples
    naab_lexer
    naab_parser
    naab_semantic
    fmt::fmt
)
message(STATUS "  ✓ Example programs test")

# Stdlib Modules Test (Phase 4 - quirky-exploring-giraffe.md)
add_executable(test_stdlib_modules
    tests/unit/test_stdlib_modules.cpp
)
target_link_libraries(test_stdlib_modules
    naab_runtime
    naab_interpreter
    naab_parser
    naab_semantic
    naab_stdlib
    fmt::fmt
    spdlog::spdlog
)
message(STATUS "  ✓ Stdlib modules test")

# String Methods Test (Phase 2.1 - Comprehensive)
add_executable(test_string_methods
    tests/unit/test_string_methods.cpp
)
target_link_libraries(test_string_methods
    naab_runtime
    naab_interpreter
    naab_parser
    naab_semantic
    naab_stdlib
    fmt::fmt
    spdlog::spdlog
)
message(STATUS "  ✓ String methods test (Phase 2.1)")

# LSP Integration Test (Phase 4.1 - Week 1)
add_executable(lsp_integration_test
    tests/lsp/lsp_integration_test.cpp
    tools/naab-lsp/json_rpc.cpp
    tools/naab-lsp/document_manager.cpp
    tools/naab-lsp/symbol_provider.cpp
    tools/naab-lsp/hover_provider.cpp
    tools/naab-lsp/completion_provider.cpp
    tools/naab-lsp/definition_provider.cpp
)
target_include_directories(lsp_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tools/naab-lsp
    ${CMAKE_SOURCE_DIR}/external/json/single_include
)
target_link_libraries(lsp_integration_test PRIVATE
    gtest
    gtest_main
    naab_parser
    naab_semantic
    fmt::fmt
)
add_test(NAME LSPIntegrationTest COMMAND lsp_integration_test)
message(STATUS "  ✓ LSP integration test (Phase 4.1)")

# Composition Validator Test (Phase 2.4 - Comprehensive)
# DISABLED: Test file requires refactoring - BlockLoader/BlockRegistry are not mockable
#           without adding virtual interfaces. Test file was untracked in git.
# add_executable(test_composition_validator
#     tests/unit/test_composition_validator.cpp
#     src/semantic/type_system.cpp
#     src/semantic/composition_validator.cpp
# )
# target_link_libraries(test_composition_validator
#     naab_runtime
#     fmt::fmt
#     spdlog::spdlog
# )
message(STATUS "  ⚠ Composition validator test (Phase 2.4) - DISABLED (needs refactoring)")

# Cross-Language Integration Test (Simple - QuickJS only)
add_executable(test_cross_language_simple
    tests/test_cross_language_simple.cpp
)
target_link_libraries(test_cross_language_simple
    quickjs
    fmt::fmt
    pthread
    m
)
target_include_directories(test_cross_language_simple PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/quickjs-2021-03-27
)
message(STATUS "  ✓ Cross-language integration test (simple)")

# Cross-Language Bridge Direct Tests
add_executable(test_cross_language_bridge
    tests/test_cross_language_bridge.cpp
)
target_link_libraries(test_cross_language_bridge
    quickjs
    fmt::fmt
    pthread
    m
)
if(HAVE_PYBIND11)
    target_link_libraries(test_cross_language_bridge ${Python3_LIBRARIES})
    target_include_directories(test_cross_language_bridge PRIVATE ${Python3_INCLUDE_DIRS})
    target_compile_definitions(test_cross_language_bridge PRIVATE HAVE_PYBIND11=1)
endif()
target_include_directories(test_cross_language_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/quickjs-2021-03-27
)
message(STATUS "  ✓ Cross-language bridge tests")

# Cross-Language Performance Benchmarks
add_executable(test_cross_language_performance
    tests/test_cross_language_performance.cpp
)
target_link_libraries(test_cross_language_performance
    quickjs
    fmt::fmt
    pthread
    m
)
if(HAVE_PYBIND11)
    target_link_libraries(test_cross_language_performance ${Python3_LIBRARIES})
    target_include_directories(test_cross_language_performance PRIVATE ${Python3_INCLUDE_DIRS})
    target_compile_definitions(test_cross_language_performance PRIVATE HAVE_PYBIND11=1)
endif()
target_include_directories(test_cross_language_performance PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/quickjs-2021-03-27
)
message(STATUS "  ✓ Cross-language performance benchmarks")

# End-to-End Example 1: Web Scraper (Disabled - file missing)
# add_executable(example_web_scraper
#     tests/example_web_scraper.cpp
# )
# target_link_libraries(example_web_scraper
#     naab_runtime
#     naab_interpreter
#     fmt::fmt
# )
# if(HAVE_PYBIND11)
#     target_link_libraries(example_web_scraper ${Python3_LIBRARIES})
#     target_include_directories(example_web_scraper PRIVATE ${Python3_INCLUDE_DIRS})
#     target_compile_definitions(example_web_scraper PRIVATE HAVE_PYBIND11=1)
# endif()
# message(STATUS "  ✓ Example 1: Web Scraper (Python → C++ → JS)")

# End-to-End Example 2: Data Pipeline (Disabled - file missing)
# add_executable(example_data_pipeline
#     tests/example_data_pipeline.cpp
# )
# target_link_libraries(example_data_pipeline
#     naab_runtime
#     naab_interpreter
#     fmt::fmt
# )
# if(HAVE_PYBIND11)
#     target_link_libraries(example_data_pipeline ${Python3_LIBRARIES})
#     target_include_directories(example_data_pipeline PRIVATE ${Python3_INCLUDE_DIRS})
#     target_compile_definitions(example_data_pipeline PRIVATE HAVE_PYBIND11=1)
# endif()
# message(STATUS "  ✓ Example 2: Data Pipeline (Python → C++ → JS)")

# End-to-End Example 3: API Server (Disabled - file missing)
# add_executable(example_api_server
#     tests/example_api_server.cpp
# )
# target_link_libraries(example_api_server
#     naab_runtime
#     naab_interpreter
#     fmt::fmt
# )
# if(HAVE_PYBIND11)
#     target_link_libraries(example_api_server ${Python3_LIBRARIES})
#     target_include_directories(example_api_server PRIVATE ${Python3_INCLUDE_DIRS})
#     target_compile_definitions(example_api_server PRIVATE HAVE_PYBIND11=1)
# endif()
# message(STATUS "  ✓ Example 3: API Server (Python → C++ → JS)")

# Simple Working Pipeline Example (Disabled - file missing)
# add_executable(example_simple_pipeline
#     tests/example_simple_pipeline.cpp
# )
# target_link_libraries(example_simple_pipeline
#     naab_runtime
#     naab_interpreter
#     fmt::fmt
# )
# if(HAVE_PYBIND11)
#     target_link_libraries(example_simple_pipeline ${Python3_LIBRARIES})
#     target_include_directories(example_simple_pipeline PRIVATE ${Python3_INCLUDE_DIRS})
#     target_compile_definitions(example_simple_pipeline PRIVATE HAVE_PYBIND11=1)
# endif()
# message(STATUS "  ✓ Simple Pipeline Example (Working)")

# Cross-Language Integration Test (Original - all executors)
add_executable(test_cross_language
    tests/test_cross_language.cpp
)
target_link_libraries(test_cross_language
    naab_runtime
    naab_interpreter
    fmt::fmt
)
if(HAVE_PYBIND11)
    target_link_libraries(test_cross_language ${Python3_LIBRARIES})
    target_include_directories(test_cross_language PRIVATE ${Python3_INCLUDE_DIRS})
    target_compile_definitions(test_cross_language PRIVATE HAVE_PYBIND11=1)
endif()
message(STATUS "  ✓ Cross-language integration test (original)")

# Tests
enable_testing()

# Unit tests (will add later)
# add_subdirectory(tests)

message(STATUS "======================================")
message(STATUS "NAAb build configuration complete!")
message(STATUS "")
message(STATUS "To build:")
message(STATUS "  mkdir -p build && cd build")
message(STATUS "  cmake ..")
message(STATUS "  make -j$(nproc)")
message(STATUS "")
message(STATUS "To run:")
message(STATUS "  ./naab-lang run ../examples/hello_world.naab")
message(STATUS "  ./naab-repl")
message(STATUS "======================================")

# Library Detection Test
add_executable(test_library_detection
    tests/unit/test_library_detection.cpp
)
target_link_libraries(test_library_detection
    naab_block_enricher
    fmt::fmt
)
target_include_directories(test_library_detection PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include
)
message(STATUS "  ✓ Library detection test")

# C++ Compilation Test Suite
add_executable(test_cpp_compilation_suite
    tests/unit/test_cpp_compilation_suite.cpp
)
target_link_libraries(test_cpp_compilation_suite
    naab_runtime
    naab_interpreter
    naab_parser
    naab_semantic
    naab_stdlib
    naab_block_enricher
    fmt::fmt
    spdlog::spdlog
)
if(Python3_FOUND)
    target_link_libraries(test_cpp_compilation_suite ${Python3_LIBRARIES})
    target_include_directories(test_cpp_compilation_suite PRIVATE ${Python3_INCLUDE_DIRS})
endif()
message(STATUS "  ✓ C++ compilation test suite")

# Phase 4.1: Exception handling test
add_executable(test_exception_handling
    tests/unit/test_exception_handling.cpp
)
target_link_libraries(test_exception_handling
    naab_runtime
    naab_interpreter
    naab_parser
    naab_lexer
    fmt::fmt
)
message(STATUS "  ✓ Exception handling test")

# ============================================================================
# GoogleTest Integration
# ============================================================================
message(STATUS "Configuring GoogleTest...")

# Disable GoogleTest warnings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# Add GoogleTest
add_subdirectory(external/googletest EXCLUDE_FROM_ALL)

enable_testing()

# ============================================================================
# Unit Tests with GoogleTest
# ============================================================================
message(STATUS "Configuring unit tests...")

# Create test executable
add_executable(naab_unit_tests
    tests/unit/lexer_test.cpp
    tests/unit/parser_test.cpp
    tests/unit/interpreter_test.cpp
    tests/unit/type_system_test.cpp
    tests/unit/stdlib_test.cpp
    tests/unit/struct_test.cpp
    tests/unit/interpreter_struct_test.cpp
    tests/unit/parser_struct_test.cpp
    tests/unit/rust_ffi_test.cpp
    tests/unit/error_categories_test.cpp
    tests/unit/suggestion_system_test.cpp
    tests/unit/stack_tracer_test.cpp
    tests/unit/safe_time_test.cpp
    tests/unit/secure_string_test.cpp
    tests/unit/safe_regex_test.cpp  # Phase 1 Item 7: Regex timeout tests
    tests/unit/tamper_evident_logger_test.cpp  # Phase 1 Item 8: Tamper-evident logging tests
    tests/unit/ffi_callback_validator_test.cpp  # Phase 1 Item 9: FFI callback safety tests
    tests/unit/ffi_async_callback_test.cpp  # Phase 1 Item 10 Day 4: FFI async callback safety tests
    tests/unit/polyglot_async_test.cpp  # Phase 1 Item 10 Day 5: Polyglot async integration tests
)

# Link GoogleTest and NAAb libraries
target_link_libraries(naab_unit_tests
    gtest_main
    gtest
    naab_lexer
    naab_parser
    naab_interpreter
    naab_runtime
    naab_stdlib
    naab_semantic
    naab_security  # For SafeRegex tests
    naab_manifest  # For manifest loader
    fmt::fmt
    spdlog::spdlog
)

# Add test discovery
# Note: gtest_discover_tests disabled due to Termux permission issues
# include(GoogleTest)
# gtest_discover_tests(naab_unit_tests)

message(STATUS "  ✓ Unit tests configured")
message(STATUS "  Run tests with: ./build/naab_unit_tests")

# ============================================================================
# Fuzzing Infrastructure (Week 2: Security Hardening Sprint)
# ============================================================================
# add_subdirectory(fuzz)  # Commented out - fuzz directory removed during cleanup

