#pragma once

#include <string>
#include <vector>

namespace naab {
namespace analyzer {

/**
 * Task Intent Categories
 *
 * Represents the inferred purpose of code
 */
enum class TaskIntent {
    // Computation intents
    NUMERICAL_COMPUTATION,     // Heavy math operations
    STATISTICAL_ANALYSIS,      // Stats operations
    LINEAR_ALGEBRA,            // Matrix operations
    SIGNAL_PROCESSING,         // FFT, convolution
    MACHINE_LEARNING,          // ML algorithms

    // Data manipulation intents
    STRING_MANIPULATION,       // Text processing
    DATA_TRANSFORMATION,       // Map/filter/reduce
    DATA_PARSING,              // JSON/XML/CSV parsing
    DATA_SERIALIZATION,        // Encode/decode

    // I/O intents
    FILE_OPERATIONS,           // Filesystem access
    NETWORK_COMMUNICATION,     // HTTP/sockets
    DATABASE_ACCESS,           // SQL queries
    STREAM_PROCESSING,         // Read/write streams

    // System intents
    SYSTEMS_PROGRAMMING,       // Low-level ops
    MEMORY_MANAGEMENT,         // Alloc/free
    PROCESS_CONTROL,           // Fork/exec
    INTEROP,                   // FFI/syscalls

    // Concurrency intents
    ASYNC_OPERATIONS,          // async/await
    PARALLEL_COMPUTATION,      // Multi-thread
    DISTRIBUTED_COMPUTING,     // Multi-process

    // Control intents
    CLI_TOOL,                  // Command-line tool
    WEB_SERVICE,               // HTTP server
    BATCH_PROCESSING,          // Data pipeline
    EVENT_HANDLING,            // Event loop

    // General
    UNKNOWN                    // Could not determine
};

/**
 * Computational Profile
 *
 * Describes computational characteristics of code
 */
struct ComputationalProfile {
    bool is_cpu_intensive = false;       // Heavy computation
    bool is_memory_intensive = false;    // Large data structures
    bool is_io_intensive = false;        // Frequent I/O
    bool is_latency_sensitive = false;   // Real-time needs
    bool is_throughput_focused = false;  // Batch processing
};

/**
 * Data Flow Pattern
 *
 * Describes how data moves through code
 */
struct DataFlowPattern {
    bool is_pipeline = false;           // Linear flow
    bool is_scatter_gather = false;     // Fork/join
    bool is_map_reduce = false;         // Functional
    bool is_streaming = false;          // Incremental
    bool is_batch = false;              // All-at-once
};

/**
 * Performance Priority
 */
enum class PerformancePriority {
    STARTUP_TIME,      // Fast launch
    EXECUTION_SPEED,   // Fast runtime
    MEMORY_USAGE,      // Low memory
    DEVELOPER_TIME,    // Quick to write
    MAINTAINABILITY    // Easy to modify
};

/**
 * Performance Criteria
 *
 * What matters most for this code
 */
struct PerformanceCriteria {
    std::vector<PerformancePriority> priorities;
};

// Helper functions
std::string taskIntentToString(TaskIntent intent);
TaskIntent stringToTaskIntent(const std::string& str);

} // namespace analyzer
} // namespace naab
