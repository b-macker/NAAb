#ifndef NAAB_BLOCK_VALUE_H
#define NAAB_BLOCK_VALUE_H

#include <memory>
#include <string>
#include <variant>
#include "naab/runtime/block_loader.h"
#include "naab/runtime/executor.h"

// GEMINI FIX: Forward declaration of interpreter::Value
namespace naab {
namespace interpreter {
    class Value;
}
}

namespace naab {
namespace interpreter {

// Block wrapper for loaded blocks (multi-language support)
struct BlockValue {
    naab::runtime::BlockMetadata metadata;
    std::string code;
    std::string python_namespace;  // Python namespace name for this block (Python only)
    std::string member_path;        // For member access like "Class.method"

    // Phase 7: Executor support (either owned or borrowed)
    naab::runtime::Executor* executor_;              // Borrowed executor (for shared executors like JS)
    std::unique_ptr<naab::runtime::Executor> owned_executor_;  // Owned executor (for C++ blocks)

    BlockValue(const naab::runtime::BlockMetadata& meta, const std::string& c,
               const std::string& ns = "", const std::string& mp = "")
        : metadata(meta), code(c), python_namespace(ns), member_path(mp), executor_(nullptr) {}

    // Phase 7: Constructor with borrowed executor (for JS, Python, etc.)
    BlockValue(const naab::runtime::BlockMetadata& meta, const std::string& c,
               naab::runtime::Executor* exec)
        : metadata(meta), code(c), python_namespace(""), member_path(""), executor_(exec) {}

    // Phase 7: Constructor with owned executor (for C++)
    BlockValue(const naab::runtime::BlockMetadata& meta, const std::string& c,
               std::unique_ptr<naab::runtime::Executor> exec)
        : metadata(meta), code(c), python_namespace(""), member_path(""),
          executor_(nullptr), owned_executor_(std::move(exec)) {}

    // Phase 7: Get active executor (owned or borrowed)
    naab::runtime::Executor* getExecutor() const {
        return owned_executor_ ? owned_executor_.get() : executor_;
    }
};

} // namespace interpreter
} // namespace naab

#endif // NAAB_BLOCK_VALUE_H
