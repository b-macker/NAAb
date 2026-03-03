#include "naab/analyzer/detection_types.h"
#include <stdexcept>

namespace naab {
namespace analyzer {

std::string taskIntentToString(TaskIntent intent) {
    switch (intent) {
        // Computation intents
        case TaskIntent::NUMERICAL_COMPUTATION:
            return "numerical_computation";
        case TaskIntent::STATISTICAL_ANALYSIS:
            return "statistical_analysis";
        case TaskIntent::LINEAR_ALGEBRA:
            return "linear_algebra";
        case TaskIntent::SIGNAL_PROCESSING:
            return "signal_processing";
        case TaskIntent::MACHINE_LEARNING:
            return "machine_learning";

        // Data manipulation intents
        case TaskIntent::STRING_MANIPULATION:
            return "string_manipulation";
        case TaskIntent::DATA_TRANSFORMATION:
            return "data_transformation";
        case TaskIntent::DATA_PARSING:
            return "data_parsing";
        case TaskIntent::DATA_SERIALIZATION:
            return "data_serialization";

        // I/O intents
        case TaskIntent::FILE_OPERATIONS:
            return "file_operations";
        case TaskIntent::NETWORK_COMMUNICATION:
            return "network_communication";
        case TaskIntent::DATABASE_ACCESS:
            return "database_access";
        case TaskIntent::STREAM_PROCESSING:
            return "stream_processing";

        // System intents
        case TaskIntent::SYSTEMS_PROGRAMMING:
            return "systems_programming";
        case TaskIntent::MEMORY_MANAGEMENT:
            return "memory_management";
        case TaskIntent::PROCESS_CONTROL:
            return "process_control";
        case TaskIntent::INTEROP:
            return "interop";

        // Concurrency intents
        case TaskIntent::ASYNC_OPERATIONS:
            return "async_operations";
        case TaskIntent::PARALLEL_COMPUTATION:
            return "parallel_computation";
        case TaskIntent::DISTRIBUTED_COMPUTING:
            return "distributed_computing";

        // Control intents
        case TaskIntent::CLI_TOOL:
            return "cli_tool";
        case TaskIntent::WEB_SERVICE:
            return "web_service";
        case TaskIntent::BATCH_PROCESSING:
            return "batch_processing";
        case TaskIntent::EVENT_HANDLING:
            return "event_handling";

        case TaskIntent::UNKNOWN:
        default:
            return "unknown";
    }
}

TaskIntent stringToTaskIntent(const std::string& str) {
    // Computation intents
    if (str == "numerical_computation") return TaskIntent::NUMERICAL_COMPUTATION;
    if (str == "statistical_analysis") return TaskIntent::STATISTICAL_ANALYSIS;
    if (str == "linear_algebra") return TaskIntent::LINEAR_ALGEBRA;
    if (str == "signal_processing") return TaskIntent::SIGNAL_PROCESSING;
    if (str == "machine_learning") return TaskIntent::MACHINE_LEARNING;

    // Data manipulation intents
    if (str == "string_manipulation") return TaskIntent::STRING_MANIPULATION;
    if (str == "data_transformation") return TaskIntent::DATA_TRANSFORMATION;
    if (str == "data_parsing") return TaskIntent::DATA_PARSING;
    if (str == "data_serialization") return TaskIntent::DATA_SERIALIZATION;

    // I/O intents
    if (str == "file_operations") return TaskIntent::FILE_OPERATIONS;
    if (str == "network_communication") return TaskIntent::NETWORK_COMMUNICATION;
    if (str == "database_access") return TaskIntent::DATABASE_ACCESS;
    if (str == "stream_processing") return TaskIntent::STREAM_PROCESSING;

    // System intents
    if (str == "systems_programming") return TaskIntent::SYSTEMS_PROGRAMMING;
    if (str == "memory_management") return TaskIntent::MEMORY_MANAGEMENT;
    if (str == "process_control") return TaskIntent::PROCESS_CONTROL;
    if (str == "interop") return TaskIntent::INTEROP;

    // Concurrency intents
    if (str == "async_operations") return TaskIntent::ASYNC_OPERATIONS;
    if (str == "parallel_computation") return TaskIntent::PARALLEL_COMPUTATION;
    if (str == "distributed_computing") return TaskIntent::DISTRIBUTED_COMPUTING;

    // Control intents
    if (str == "cli_tool") return TaskIntent::CLI_TOOL;
    if (str == "web_service") return TaskIntent::WEB_SERVICE;
    if (str == "batch_processing") return TaskIntent::BATCH_PROCESSING;
    if (str == "event_handling") return TaskIntent::EVENT_HANDLING;

    return TaskIntent::UNKNOWN;
}

} // namespace analyzer
} // namespace naab
