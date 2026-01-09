#include "naab/doc_generator.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fmt/core.h>

namespace naab {
namespace doc {

bool DocGenerator::isDocComment(const std::string& line) {
    // Trim leading whitespace
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    return line[start] == '#';
}

bool DocGenerator::isFunctionDefinition(const std::string& line) {
    // Trim leading whitespace
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    return line.substr(start, 3) == "fn ";
}

std::string DocGenerator::cleanCommentLine(const std::string& line) {
    // Find the # character
    size_t hash_pos = line.find('#');
    if (hash_pos == std::string::npos) return "";

    // Get everything after #
    std::string content = line.substr(hash_pos + 1);

    // Trim leading and trailing whitespace
    size_t start = content.find_first_not_of(" \t");
    if (start == std::string::npos) return "";

    size_t end = content.find_last_not_of(" \t\r\n");
    return content.substr(start, end - start + 1);
}

std::pair<std::string, std::vector<std::string>> DocGenerator::parseFunctionSignature(
    const std::string& signature) {

    // Example: "fn add(a, b) {"
    // Extract function name and parameters

    size_t fn_pos = signature.find("fn ");
    if (fn_pos == std::string::npos) {
        return {"", {}};
    }

    // Find the opening parenthesis
    size_t paren_start = signature.find('(', fn_pos + 3);
    if (paren_start == std::string::npos) {
        return {"", {}};
    }

    // Extract function name
    std::string name = signature.substr(fn_pos + 3, paren_start - (fn_pos + 3));
    // Trim whitespace
    size_t name_start = name.find_first_not_of(" \t");
    size_t name_end = name.find_last_not_of(" \t");
    if (name_start != std::string::npos && name_end != std::string::npos) {
        name = name.substr(name_start, name_end - name_start + 1);
    }

    // Find closing parenthesis
    size_t paren_end = signature.find(')', paren_start);
    if (paren_end == std::string::npos) {
        return {name, {}};
    }

    // Extract parameters
    std::string params_str = signature.substr(paren_start + 1, paren_end - paren_start - 1);

    std::vector<std::string> params;
    if (!params_str.empty()) {
        std::stringstream ss(params_str);
        std::string param;
        while (std::getline(ss, param, ',')) {
            // Trim whitespace
            size_t start = param.find_first_not_of(" \t");
            size_t end = param.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                params.push_back(param.substr(start, end - start + 1));
            }
        }
    }

    return {name, params};
}

FunctionDoc DocGenerator::parseDocComment(
    const std::vector<std::string>& comment_lines,
    const std::string& function_signature,
    int line_number) {

    FunctionDoc doc;
    doc.line_number = line_number;

    // Parse function signature
    auto [name, params] = parseFunctionSignature(function_signature);
    doc.name = name;
    doc.parameters = params;

    // Parse comment lines
    std::string current_description;
    for (const auto& line : comment_lines) {
        std::string cleaned = cleanCommentLine(line);
        if (cleaned.empty()) continue;

        // Check for @param tag
        if (cleaned.find("@param ") == 0) {
            // Format: "@param name description"
            std::string rest = cleaned.substr(7);  // Skip "@param "
            size_t space_pos = rest.find(' ');
            if (space_pos != std::string::npos) {
                std::string param_name = rest.substr(0, space_pos);
                std::string param_desc = rest.substr(space_pos + 1);
                doc.param_docs[param_name] = param_desc;
            }
        }
        // Check for @return tag
        else if (cleaned.find("@return ") == 0) {
            doc.return_doc = cleaned.substr(8);  // Skip "@return "
        }
        // Check for @example tag
        else if (cleaned.find("@example ") == 0) {
            doc.example = cleaned.substr(9);  // Skip "@example "
        }
        // Otherwise it's part of the description
        else {
            if (!current_description.empty()) {
                current_description += " ";
            }
            current_description += cleaned;
        }
    }

    doc.description = current_description;
    return doc;
}

ModuleDoc DocGenerator::parseFile(const std::string& filepath) {
    ModuleDoc module;
    module.filename = filepath;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error(fmt::format("Failed to open file: {}", filepath));
    }

    std::vector<std::string> comment_buffer;
    std::string line;
    int line_number = 0;

    while (std::getline(file, line)) {
        line_number++;

        // Check if this is a doc comment
        if (isDocComment(line)) {
            comment_buffer.push_back(line);
        }
        // Check if this is a function definition
        else if (isFunctionDefinition(line)) {
            // We have a function with preceding comments
            if (!comment_buffer.empty()) {
                FunctionDoc func_doc = parseDocComment(comment_buffer, line, line_number);
                module.functions.push_back(func_doc);
            } else {
                // Function without documentation - still record it
                auto [name, params] = parseFunctionSignature(line);
                FunctionDoc func_doc;
                func_doc.name = name;
                func_doc.parameters = params;
                func_doc.line_number = line_number;
                func_doc.description = "(No documentation)";
                module.functions.push_back(func_doc);
            }
            comment_buffer.clear();
        }
        // If we hit a non-comment, non-function line, clear buffer
        else if (!line.empty() && line.find_first_not_of(" \t") != std::string::npos) {
            // Check if comments at top are module-level description
            if (module.functions.empty() && !comment_buffer.empty()) {
                // These are module-level comments
                for (const auto& comment : comment_buffer) {
                    std::string cleaned = cleanCommentLine(comment);
                    if (!cleaned.empty()) {
                        if (!module.module_description.empty()) {
                            module.module_description += " ";
                        }
                        module.module_description += cleaned;
                    }
                }
            }
            comment_buffer.clear();
        }
    }

    return module;
}

std::string DocGenerator::generateMarkdown(const ModuleDoc& module_doc) {
    std::stringstream ss;

    // Title
    size_t last_slash = module_doc.filename.find_last_of("/\\");
    std::string filename = (last_slash != std::string::npos)
        ? module_doc.filename.substr(last_slash + 1)
        : module_doc.filename;

    ss << "# " << filename << "\n\n";

    // Module description
    if (!module_doc.module_description.empty()) {
        ss << module_doc.module_description << "\n\n";
    }

    // Table of contents
    if (!module_doc.functions.empty()) {
        ss << "## Functions\n\n";
        for (const auto& func : module_doc.functions) {
            ss << "- [" << func.name << "](#" << func.name << ")\n";
        }
        ss << "\n---\n\n";
    }

    // Function documentation
    for (const auto& func : module_doc.functions) {
        // Function header
        ss << "## " << func.name << "(";
        for (size_t i = 0; i < func.parameters.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << func.parameters[i];
        }
        ss << ")\n\n";

        // Description
        if (!func.description.empty()) {
            ss << func.description << "\n\n";
        }

        // Parameters
        if (!func.parameters.empty()) {
            ss << "**Parameters:**\n";
            for (const auto& param : func.parameters) {
                ss << "- `" << param << "`";
                auto it = func.param_docs.find(param);
                if (it != func.param_docs.end()) {
                    ss << " - " << it->second;
                }
                ss << "\n";
            }
            ss << "\n";
        }

        // Return value
        if (!func.return_doc.empty()) {
            ss << "**Returns:** " << func.return_doc << "\n\n";
        }

        // Example
        if (!func.example.empty()) {
            ss << "**Example:**\n```naab\n" << func.example << "\n```\n\n";
        }

        // Source location
        ss << "*Defined in " << filename << " at line " << func.line_number << "*\n\n";
        ss << "---\n\n";
    }

    return ss.str();
}

std::string DocGenerator::generateCatalog(const std::vector<ModuleDoc>& modules) {
    std::stringstream ss;

    ss << "# NAAb API Documentation\n\n";
    ss << "This is an automatically generated catalog of all documented functions.\n\n";

    // Summary statistics
    int total_functions = 0;
    int documented_functions = 0;
    for (const auto& module : modules) {
        total_functions += module.functions.size();
        for (const auto& func : module.functions) {
            if (!func.description.empty() && func.description != "(No documentation)") {
                documented_functions++;
            }
        }
    }

    ss << "**Statistics:**\n";
    ss << "- Modules: " << modules.size() << "\n";
    ss << "- Total Functions: " << total_functions << "\n";
    ss << "- Documented Functions: " << documented_functions << "\n";
    ss << "- Documentation Coverage: "
       << (total_functions > 0 ? (documented_functions * 100 / total_functions) : 0)
       << "%\n\n";

    ss << "---\n\n";

    // Module index
    ss << "## Modules\n\n";
    for (const auto& module : modules) {
        size_t last_slash = module.filename.find_last_of("/\\");
        std::string filename = (last_slash != std::string::npos)
            ? module.filename.substr(last_slash + 1)
            : module.filename;

        ss << "### " << filename << "\n";
        if (!module.module_description.empty()) {
            ss << module.module_description << "\n";
        }
        ss << "\n**Functions:** " << module.functions.size() << "\n";

        // List functions
        for (const auto& func : module.functions) {
            ss << "- `" << func.name << "(";
            for (size_t i = 0; i < func.parameters.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << func.parameters[i];
            }
            ss << ")` - " << func.description << "\n";
        }
        ss << "\n";
    }

    ss << "---\n\n";

    // Alphabetical function index
    ss << "## All Functions (Alphabetical)\n\n";

    // Collect all functions
    std::vector<std::pair<std::string, const FunctionDoc*>> all_functions;
    for (const auto& module : modules) {
        for (const auto& func : module.functions) {
            all_functions.push_back({module.filename, &func});
        }
    }

    // Sort alphabetically
    std::sort(all_functions.begin(), all_functions.end(),
        [](const auto& a, const auto& b) {
            return a.second->name < b.second->name;
        });

    // Print index
    for (const auto& [filepath, func] : all_functions) {
        size_t last_slash = filepath.find_last_of("/\\");
        std::string filename = (last_slash != std::string::npos)
            ? filepath.substr(last_slash + 1)
            : filepath;

        ss << "- **" << func->name << "** (";
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << func->parameters[i];
        }
        ss << ") - " << func->description << " *[" << filename << "]*\n";
    }
    ss << "\n";

    return ss.str();
}

} // namespace doc
} // namespace naab
