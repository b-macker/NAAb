// NAAb Composition Validator Implementation - Phase 2.4
// Validates block chains and suggests adapter blocks

#include "naab/composition_validator.h"
#include <fmt/core.h>
#include <sstream>
#include <algorithm>

namespace naab {
namespace validator {

using runtime::BlockMetadata;
using runtime::BlockLoader;

// ============================================================================
// CompositionError Implementation
// ============================================================================

std::string CompositionError::formatMessage() const {
    std::stringstream ss;
    ss << "Type mismatch at position " << position << ":\n";
    ss << "  Block '" << source_block_id << "' outputs: " << actual_type.toString() << "\n";
    ss << "  Block '" << target_block_id << "' expects: " << expected_type.toString() << "\n";

    if (!suggested_adapters.empty()) {
        ss << "  Suggested adapters: ";
        for (size_t i = 0; i < suggested_adapters.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << suggested_adapters[i];
        }
        ss << "\n";
    }

    return ss.str();
}

// ============================================================================
// CompositionValidation Implementation
// ============================================================================

void CompositionValidation::addError(CompositionError error) {
    is_valid = false;
    errors.push_back(std::move(error));
}

std::string CompositionValidation::getReport() const {
    std::stringstream ss;

    if (is_valid) {
        ss << "✓ Composition is valid\n";
        ss << "  Block chain: ";
        for (size_t i = 0; i < block_chain.size(); ++i) {
            if (i > 0) ss << " -> ";
            ss << block_chain[i];
        }
        ss << "\n";

        ss << "  Type flow: ";
        for (size_t i = 0; i < type_flow.size(); ++i) {
            if (i > 0) ss << " -> ";
            ss << type_flow[i].toString();
        }
        ss << "\n";
    } else {
        ss << "✗ Composition is invalid (" << errors.size() << " error(s))\n\n";
        for (size_t i = 0; i < errors.size(); ++i) {
            ss << "Error " << (i + 1) << ":\n";
            ss << errors[i].formatMessage();
            if (i < errors.size() - 1) {
                ss << "\n";
            }
        }
    }

    return ss.str();
}

std::optional<std::string> CompositionValidation::getSuggestedFix() const {
    if (errors.empty()) {
        return std::nullopt;
    }

    const auto& first_error = errors[0];
    if (first_error.suggested_adapters.empty()) {
        return std::nullopt;
    }

    std::stringstream ss;
    ss << "Insert '" << first_error.suggested_adapters[0] << "' ";
    ss << "between '" << first_error.source_block_id << "' ";
    ss << "and '" << first_error.target_block_id << "'";

    return ss.str();
}

// ============================================================================
// CompositionValidator Implementation
// ============================================================================

CompositionValidator::CompositionValidator(std::shared_ptr<BlockLoader> loader)
    : loader_(std::move(loader)), strict_mode_(false) {}

CompositionValidation CompositionValidator::validate(
    const std::vector<std::string>& block_ids) {

    CompositionValidation result;
    result.block_chain = block_ids;

    if (block_ids.empty()) {
        return result;  // Empty chain is valid
    }

    if (block_ids.size() == 1) {
        // Single block is always valid
        try {
            auto output_type = getBlockOutputType(block_ids[0]);
            if (output_type) {
                result.type_flow.push_back(*output_type);
            }
        } catch (const std::exception& e) {
            // Block not found, but single block is still structurally valid
        }
        return result;
    }

    // Validate each adjacent pair
    for (size_t i = 0; i < block_ids.size() - 1; ++i) {
        try {
            auto source_block = loader_->getBlock(block_ids[i]);
            auto target_block = loader_->getBlock(block_ids[i + 1]);

            // Record type flow
            if (i == 0) {
                auto input_type = getBlockInputType(block_ids[i]);
                if (input_type) {
                    result.type_flow.push_back(*input_type);
                }
            }

            auto output_type = getBlockOutputType(block_ids[i]);
            if (output_type) {
                result.type_flow.push_back(*output_type);
            }

            // Validate compatibility
            auto error = validateStep(source_block, target_block, i);
            if (error) {
                result.addError(*error);
            }
        } catch (const std::exception& e) {
            result.is_valid = false;
            // Block not found in loader
            continue;
        }
    }

    // Add final output type
    auto final_output = getBlockOutputType(block_ids.back());
    if (final_output) {
        result.type_flow.push_back(*final_output);
    }

    return result;
}

bool CompositionValidator::canChain(const std::string& source_id,
                                     const std::string& target_id) {
    try {
        auto source_block = loader_->getBlock(source_id);
        auto target_block = loader_->getBlock(target_id);

        auto error = validateStep(source_block, target_block, 0);
        return !error.has_value();
    } catch (const std::exception& e) {
        return false;
    }
}

std::vector<std::string> CompositionValidator::suggestAdapter(
    const types::Type& source_type,
    const types::Type& target_type) {

    std::vector<std::string> suggestions;

    // Get all blocks from registry
    auto all_adapters = getAllAdapters();

    for (const auto& adapter : all_adapters) {
        auto adapter_input = parseTypeFromMetadata(adapter.input_types);
        auto adapter_output = parseTypeFromMetadata(adapter.output_type);

        if (!adapter_input || !adapter_output) {
            continue;
        }

        // Check if adapter can convert source_type to target_type
        if (source_type.isCompatibleWith(*adapter_input) &&
            adapter_output->isCompatibleWith(target_type)) {
            suggestions.push_back(adapter.block_id);
        }
    }

    return suggestions;
}

std::vector<std::string> CompositionValidator::suggestAdapterForBlocks(
    const std::string& source_id,
    const std::string& target_id) {

    auto source_type = getBlockOutputType(source_id);
    auto target_type = getBlockInputType(target_id);

    if (!source_type || !target_type) {
        return {};
    }

    return suggestAdapter(*source_type, *target_type);
}

std::optional<CompositionError> CompositionValidator::validateStep(
    const BlockMetadata& source_block,
    const BlockMetadata& target_block,
    size_t position) {

    auto source_output = parseTypeFromMetadata(source_block.output_type);
    auto target_input = parseTypeFromMetadata(target_block.input_types);

    if (!source_output || !target_input) {
        // If we can't parse types, we can't validate
        // In non-strict mode, allow it
        if (!strict_mode_) {
            return std::nullopt;
        }
    }

    // Check compatibility
    if (source_output && target_input) {
        if (!source_output->isCompatibleWith(*target_input)) {
            CompositionError error(
                position,
                source_block.block_id,
                target_block.block_id,
                *target_input,
                *source_output
            );

            // Find suggested adapters
            error.suggested_adapters = suggestAdapter(*source_output, *target_input);

            return error;
        }
    }

    return std::nullopt;
}

std::optional<types::Type> CompositionValidator::getBlockOutputType(
    const std::string& block_id) {

    try {
        auto block = loader_->getBlock(block_id);
        return parseTypeFromMetadata(block.output_type);
    } catch (const std::exception& e) {
        return std::nullopt;
    }
}

std::optional<types::Type> CompositionValidator::getBlockInputType(
    const std::string& block_id) {

    try {
        auto block = loader_->getBlock(block_id);
        return parseTypeFromMetadata(block.input_types);
    } catch (const std::exception& e) {
        return std::nullopt;
    }
}

std::optional<types::Type> CompositionValidator::parseTypeFromMetadata(
    const std::string& type_str) {

    if (type_str.empty()) {
        return types::Type::Any();  // No type specified = any
    }

    return types::Type::parse(type_str);
}

bool CompositionValidator::isAdapter(const BlockMetadata& block) {
    // An adapter typically:
    // - Has "adapter" or "convert" in name or category
    // - Takes one type and outputs a different type
    // - Category includes "type_conversion" or "adapter"

    std::string lower_name = block.name;
    std::string lower_category = block.category;

    std::transform(lower_name.begin(), lower_name.end(),
                   lower_name.begin(), ::tolower);
    std::transform(lower_category.begin(), lower_category.end(),
                   lower_category.begin(), ::tolower);

    return lower_name.find("adapt") != std::string::npos ||
           lower_name.find("convert") != std::string::npos ||
           lower_category.find("adapter") != std::string::npos ||
           lower_category.find("type_conversion") != std::string::npos ||
           lower_category.find("transform") != std::string::npos;
}

std::vector<BlockMetadata> CompositionValidator::getAllAdapters() {
    std::vector<BlockMetadata> adapters;

    // Search for adapter blocks
    // This would ideally use BlockRegistry's search functionality
    // For now, we return empty (would be populated from database in real usage)

    // TODO: Implement registry->searchByCategory("adapter") or similar
    // For MVP, adapter detection is based on type compatibility

    return adapters;
}

// ============================================================================
// Utility Functions
// ============================================================================

std::string formatTypeMismatch(const types::Type& expected,
                                const types::Type& actual,
                                const std::string& context) {
    std::stringstream ss;

    if (!context.empty()) {
        ss << context << ": ";
    }

    ss << "expected '" << expected.toString() << "', ";
    ss << "got '" << actual.toString() << "'";

    // Add helpful hint for common mistakes
    if (expected.isNumeric() && actual.isNumeric()) {
        ss << " (numeric types are compatible)";
    } else if (expected.getBase() == types::BaseType::Any) {
        ss << " (any type accepted)";
    } else if (actual.getBase() == types::BaseType::Any) {
        ss << " (produces any type)";
    }

    return ss.str();
}

std::string formatAdapterSuggestion(const std::vector<std::string>& adapters) {
    if (adapters.empty()) {
        return "No adapter blocks found for this type conversion.";
    }

    std::stringstream ss;
    ss << "Try inserting one of these adapter blocks:\n";
    for (size_t i = 0; i < adapters.size(); ++i) {
        ss << "  " << (i + 1) << ". " << adapters[i];
        if (i < adapters.size() - 1) {
            ss << "\n";
        }
    }

    return ss.str();
}

} // namespace validator
} // namespace naab
