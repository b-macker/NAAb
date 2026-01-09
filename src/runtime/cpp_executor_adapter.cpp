// NAAb C++ Executor Adapter Implementation
// Adapts CppExecutor to the Executor interface

#include "naab/cpp_executor_adapter.h"
#include "naab/block_enricher.h"
#include <fmt/core.h>

namespace naab {
namespace runtime {

CppExecutorAdapter::CppExecutorAdapter()
    : block_counter_(0) {
    fmt::print("[CPP ADAPTER] C++ executor adapter initialized\n");
}

bool CppExecutorAdapter::execute(const std::string& code) {
    // Generate unique block ID
    current_block_id_ = fmt::format("CPP-BLOCK-{}", ++block_counter_);

    fmt::print("[CPP ADAPTER] Compiling C++ block: {}\n", current_block_id_);

    // Detect required libraries from code
    tools::BlockEnricher enricher;
    std::vector<std::string> libraries = enricher.detectLibraries(code);

    if (!libraries.empty()) {
        fmt::print("[CPP ADAPTER] Detected libraries:");
        for (const auto& lib : libraries) {
            fmt::print(" {}", lib);
        }
        fmt::print("\n");
    }

    // Compile the block with detected libraries
    return executor_.compileBlock(current_block_id_, code, "execute", libraries);
}

std::shared_ptr<interpreter::Value> CppExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (current_block_id_.empty()) {
        throw std::runtime_error("No C++ block loaded. Call execute() first.");
    }

    fmt::print("[CPP ADAPTER] Calling function: {}\n", function_name);

    // Call function in the current block
    return executor_.callFunction(current_block_id_, function_name, args);
}

bool CppExecutorAdapter::isInitialized() const {
    // C++ executor is always initialized (no runtime needed)
    return true;
}

} // namespace runtime
} // namespace naab
