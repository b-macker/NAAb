#ifndef NAAB_DOC_GENERATOR_H
#define NAAB_DOC_GENERATOR_H

#include <string>
#include <vector>
#include <unordered_map>

namespace naab {
namespace doc {

/**
 * Represents a function parameter
 */
struct Parameter {
    std::string name;
    std::string description;
};

/**
 * Represents a documented function
 */
struct FunctionDoc {
    std::string name;
    std::vector<std::string> parameters;  // Parameter names
    std::string description;
    std::unordered_map<std::string, std::string> param_docs;  // param name -> description
    std::string return_doc;
    std::string example;
    int line_number;
};

/**
 * Represents a module/file with its functions
 */
struct ModuleDoc {
    std::string filename;
    std::string module_description;
    std::vector<FunctionDoc> functions;
};

/**
 * Documentation generator for NAAb source files
 */
class DocGenerator {
public:
    DocGenerator() = default;

    /**
     * Parse a NAAb source file and extract documentation
     * @param filepath Path to .naab file
     * @return ModuleDoc with extracted documentation
     */
    ModuleDoc parseFile(const std::string& filepath);

    /**
     * Generate markdown documentation from ModuleDoc
     * @param module_doc The module documentation
     * @return Markdown string
     */
    std::string generateMarkdown(const ModuleDoc& module_doc);

    /**
     * Generate a catalog/index of multiple modules
     * @param modules Vector of module docs
     * @return Markdown string for catalog
     */
    std::string generateCatalog(const std::vector<ModuleDoc>& modules);

private:
    /**
     * Parse doc comments from lines preceding a function
     * @param lines Vector of comment lines
     * @return FunctionDoc with parsed metadata
     */
    FunctionDoc parseDocComment(const std::vector<std::string>& comment_lines,
                                const std::string& function_signature,
                                int line_number);

    /**
     * Extract function signature components
     * @param signature The function signature line
     * @return Pair of function name and parameter list
     */
    std::pair<std::string, std::vector<std::string>> parseFunctionSignature(
        const std::string& signature);

    /**
     * Clean and format a comment line (remove # prefix, trim)
     */
    std::string cleanCommentLine(const std::string& line);

    /**
     * Check if a line is a doc comment
     */
    bool isDocComment(const std::string& line);

    /**
     * Check if a line is a function definition
     */
    bool isFunctionDefinition(const std::string& line);
};

} // namespace doc
} // namespace naab

#endif // NAAB_DOC_GENERATOR_H
