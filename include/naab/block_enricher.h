#ifndef NAAB_BLOCK_ENRICHER_H
#define NAAB_BLOCK_ENRICHER_H

// Block Enricher
// Converts code snippets to callable functions with C-ABI wrappers

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace naab {
namespace tools {

// Function signature extracted from code
struct FunctionSignature {
    std::string function_name;
    std::string return_type;
    std::vector<std::pair<std::string, std::string>> parameters;  // (type, name)

    FunctionSignature() = default;
    FunctionSignature(const std::string& name, const std::string& ret)
        : function_name(name), return_type(ret) {}
};

// Block interface metadata
struct BlockInterface {
    std::string function;
    std::vector<std::unordered_map<std::string, std::string>> parameters;
    std::unordered_map<std::string, std::string> returns;

    std::string toJSON() const;
    static BlockInterface fromSignature(const FunctionSignature& sig);
};

// Wrapper generation result
struct WrapperResult {
    std::string full_code;      // Complete compilable code with wrapper
    FunctionSignature signature;
    bool success;
    std::string error_message;

    WrapperResult() : success(false) {}
};

// Context extracted from source file
struct SourceContext {
    std::vector<std::string> includes;
    std::vector<std::string> namespaces;
    std::vector<std::string> template_params;
    std::string enclosing_class;

    bool has_context() const {
        return !includes.empty() || !namespaces.empty();
    }
};

// Block metadata from JSON
struct BlockMetadata {
    std::string id;
    std::string language;
    std::string code;
    std::string source_file;
    int source_line;
    std::string validation_status;

    BlockMetadata() : source_line(0) {}
};

// Main BlockEnricher class
class BlockEnricher {
public:
    BlockEnricher();

    // Enrich a single block
    BlockMetadata enrichBlock(const BlockMetadata& original);

    // Extract context from source file around line number
    SourceContext extractContext(const std::string& source_file, int source_line);

    // Generate C-ABI wrapper for code snippet
    WrapperResult generateWrapper(
        const std::string& code,
        const SourceContext& context,
        const std::string& block_id
    );

    // Detect if code is a complete function or just a snippet
    bool isCompleteFunction(const std::string& code);

    // Infer function signature from code
    FunctionSignature inferSignature(const std::string& code);

    // Detect required libraries from includes
    std::vector<std::string> detectLibraries(const std::string& code);

    // Detect include paths needed
    std::vector<std::string> detectIncludePaths(const std::string& code);

private:
    // Helper methods
    std::vector<std::string> extractIncludes(const std::string& file_content, int line_num);
    std::vector<std::string> extractNamespaces(const std::string& file_content, int line_num);
    std::string extractEnclosingClass(const std::string& file_content, int line_num);
    std::string generateWrapperFunction(
        const std::string& snippet,
        const FunctionSignature& sig,
        const SourceContext& ctx
    );
};

} // namespace tools
} // namespace naab

#endif // NAAB_BLOCK_ENRICHER_H
