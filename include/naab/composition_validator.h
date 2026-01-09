#ifndef NAAB_COMPOSITION_VALIDATOR_H
#define NAAB_COMPOSITION_VALIDATOR_H

// NAAb Composition Validator - Phase 2.4
// Validates block chains and suggests adapter blocks for type mismatches

#include "naab/type_system.h"
#include "naab/block_loader.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace naab {
namespace validator {

// ============================================================================
// CompositionError - Represents a type mismatch in block chain
// ============================================================================
struct CompositionError {
    size_t position;               // Position in block chain (0-based)
    std::string source_block_id;   // Block producing output
    std::string target_block_id;   // Block expecting input
    types::Type expected_type;     // Type expected by target
    types::Type actual_type;       // Type produced by source
    std::string message;           // Human-readable error message
    std::vector<std::string> suggested_adapters;  // Blocks that can adapt types

    CompositionError(size_t pos, std::string src, std::string tgt,
                     types::Type exp, types::Type act)
        : position(pos), source_block_id(std::move(src)),
          target_block_id(std::move(tgt)), expected_type(std::move(exp)),
          actual_type(std::move(act)) {
        message = formatMessage();
    }

    std::string formatMessage() const;
};

// ============================================================================
// CompositionValidation - Result of validation
// ============================================================================
struct CompositionValidation {
    bool is_valid;                           // Overall validation status
    std::vector<CompositionError> errors;    // All errors found
    std::vector<std::string> block_chain;    // Original block IDs
    std::vector<types::Type> type_flow;      // Types at each step

    CompositionValidation() : is_valid(true) {}

    // Add error and mark invalid
    void addError(CompositionError error);

    // Get formatted validation report
    std::string getReport() const;

    // Get suggested fix for first error
    std::optional<std::string> getSuggestedFix() const;
};

// ============================================================================
// CompositionValidator - Validates block compositions
// ============================================================================
class CompositionValidator {
public:
    explicit CompositionValidator(std::shared_ptr<runtime::BlockLoader> loader);

    // Main validation method
    CompositionValidation validate(const std::vector<std::string>& block_ids);

    // Quick compatibility check between two blocks
    bool canChain(const std::string& source_id, const std::string& target_id);

    // Find adapter blocks that can convert source type to target type
    std::vector<std::string> suggestAdapter(const types::Type& source_type,
                                             const types::Type& target_type);

    // Find adapter blocks by block IDs
    std::vector<std::string> suggestAdapterForBlocks(const std::string& source_id,
                                                      const std::string& target_id);

    // Validate a single block chain step
    std::optional<CompositionError> validateStep(
        const runtime::BlockMetadata& source_block,
        const runtime::BlockMetadata& target_block,
        size_t position);

    // Get type information for a block
    std::optional<types::Type> getBlockOutputType(const std::string& block_id);
    std::optional<types::Type> getBlockInputType(const std::string& block_id);

    // Configuration
    void setStrictMode(bool strict) { strict_mode_ = strict; }
    bool isStrictMode() const { return strict_mode_; }

private:
    std::shared_ptr<runtime::BlockLoader> loader_;
    bool strict_mode_;  // If true, require exact type matches

    // Helper to parse type from string (BlockMetadata stores strings)
    std::optional<types::Type> parseTypeFromMetadata(const std::string& type_str);

    // Helper to check if block is an adapter
    bool isAdapter(const runtime::BlockMetadata& block);

    // Helper to find all adapter blocks in loader
    std::vector<runtime::BlockMetadata> getAllAdapters();
};

// ============================================================================
// Utility Functions
// ============================================================================

// Format type mismatch message
std::string formatTypeMismatch(const types::Type& expected,
                                const types::Type& actual,
                                const std::string& context = "");

// Format adapter suggestion message
std::string formatAdapterSuggestion(const std::vector<std::string>& adapters);

} // namespace validator
} // namespace naab

#endif // NAAB_COMPOSITION_VALIDATOR_H
