#ifndef NAAB_BLOCK_ENRICHER_H
#define NAAB_BLOCK_ENRICHER_H

// NAAb Block Enricher
// Analyzes code snippets and generates complete, callable C++ blocks

#include <string>
#include <vector>
#include <map>
#include <optional>

namespace naab {
namespace tools {

// Function parameter metadata
struct Parameter {
    std::string name;
    std::string type;
    bool is_const = false;
    bool is_reference = false;
    bool is_pointer = false;
};

// Function signature metadata
struct FunctionSignature {
    std::string function_name;
    std::string return_type;
    std::vector<Parameter> parameters;
    bool is_extern_c = false;
    bool is_static = false;
    bool is_inline = false;
};

// Block interface metadata (goes in JSON)
struct BlockInterface {
    std::string function;
    std::vector<std::map<std::string, std::string>> parameters;
    std::map<std::string, std::string> returns;
};

// Enrichment result
struct EnrichmentResult {
    bool success = false;
    std::string enriched_code;
    BlockInterface interface;
    std::vector<std::string> required_libraries;
    std::vector<std::string> include_paths;
    std::string error_message;
};

// Block enricher class
class BlockEnricher {
public:
    BlockEnricher();
    ~BlockEnricher() = default;

    // Enrich a single block
    EnrichmentResult enrichBlock(
        const std::string& block_id,
        const std::string& code,
        const std::string& source_file,
        int source_line,
        const std::string& language
    );

    // Extract context from source file
    std::string extractContext(
        const std::string& source_file,
        int source_line,
        int context_lines = 20
    );

    // Analyze code snippet to determine signature
    std::optional<FunctionSignature> analyzeSignature(
        const std::string& code,
        const std::string& context
    );

    // Generate C-ABI wrapper for code snippet
    std::string generateWrapper(
        const std::string& block_id,
        const std::string& code,
        const std::optional<FunctionSignature>& signature
    );

    // Detect required libraries from includes
    std::vector<std::string> detectLibraries(const std::string& code);

    // Detect include paths needed
    std::vector<std::string> detectIncludePaths(const std::string& code);

    // Convert signature to interface metadata
    BlockInterface signatureToInterface(const FunctionSignature& sig);

    // Statistics
    struct Stats {
        int total_processed = 0;
        int success_count = 0;
        int failed_count = 0;
        int signature_detected = 0;
        int wrapper_generated = 0;
    };

    Stats getStats() const { return stats_; }
    void resetStats() { stats_ = Stats(); }

private:
    Stats stats_;

    // Helper: Extract function name from code
    std::string extractFunctionName(const std::string& code);

    // Helper: Detect return type from context
    std::string detectReturnType(const std::string& code, const std::string& context);

    // Helper: Extract parameters from function declaration
    std::vector<Parameter> extractParameters(const std::string& declaration);

    // Helper: Clean up code snippet
    std::string cleanCodeSnippet(const std::string& code);

    // Helper: Determine if code is already complete
    bool isCompleteFunction(const std::string& code);

    // Library detection mappings
    std::map<std::string, std::string> library_map_;
    std::map<std::string, std::string> include_path_map_;

    void initializeLibraryMappings();
};

} // namespace tools
} // namespace naab

#endif // NAAB_BLOCK_ENRICHER_H
