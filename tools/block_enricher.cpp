// Block Enricher Implementation
// Converts code snippets to callable functions

#include "naab/block_enricher.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <set>

namespace naab {
namespace tools {

// ============================================================================
// BlockInterface Implementation
// ============================================================================

std::string BlockInterface::toJSON() const {
    std::ostringstream json;
    json << "{\n";
    json << "  \"function\": \"" << function << "\",\n";
    json << "  \"parameters\": [\n";

    for (size_t i = 0; i < parameters.size(); ++i) {
        json << "    {";
        bool first = true;
        for (const auto& [key, value] : parameters[i]) {
            if (!first) json << ", ";
            json << "\"" << key << "\": \"" << value << "\"";
            first = false;
        }
        json << "}";
        if (i + 1 < parameters.size()) json << ",";
        json << "\n";
    }

    json << "  ],\n";
    json << "  \"returns\": {";
    bool first = true;
    for (const auto& [key, value] : returns) {
        if (!first) json << ", ";
        json << "\"" << key << "\": \"" << value << "\"";
        first = false;
    }
    json << "}\n";
    json << "}";

    return json.str();
}

BlockInterface BlockInterface::fromSignature(const FunctionSignature& sig) {
    BlockInterface iface;
    iface.function = sig.function_name;

    for (const auto& [type, name] : sig.parameters) {
        std::unordered_map<std::string, std::string> param;
        param["name"] = name;
        param["type"] = type;
        iface.parameters.push_back(param);
    }

    iface.returns["type"] = sig.return_type;
    return iface;
}

// ============================================================================
// BlockEnricher Implementation
// ============================================================================

BlockEnricher::BlockEnricher() {
    fmt::print("[BlockEnricher] Initialized\n");
}

BlockMetadata BlockEnricher::enrichBlock(const BlockMetadata& original) {
    fmt::print("[Enrich] Processing block: {}\n", original.id);

    BlockMetadata enriched = original;

    // Step 1: Extract context from source file
    SourceContext context;
    if (!original.source_file.empty() && original.source_line > 0) {
        context = extractContext(original.source_file, original.source_line);
    }

    // Step 2: Generate wrapper
    auto wrapper = generateWrapper(original.code, context, original.id);

    if (wrapper.success) {
        enriched.code = wrapper.full_code;
        enriched.validation_status = "validated";

        fmt::print("[Enrich] ✓ Block {} enriched successfully\n", original.id);
    } else {
        enriched.validation_status = "failed";
        fmt::print("[Enrich] ✗ Block {} failed: {}\n", original.id, wrapper.error_message);
    }

    return enriched;
}

SourceContext BlockEnricher::extractContext(const std::string& source_file, int source_line) {
    SourceContext ctx;

    // Try to open source file
    std::ifstream file(source_file);
    if (!file.is_open()) {
        fmt::print("[WARN] Cannot open source file: {}\n", source_file);
        return ctx;
    }

    // Read entire file
    std::string file_content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    file.close();

    // Extract includes, namespaces, etc.
    ctx.includes = extractIncludes(file_content, source_line);
    ctx.namespaces = extractNamespaces(file_content, source_line);
    ctx.enclosing_class = extractEnclosingClass(file_content, source_line);

    return ctx;
}

WrapperResult BlockEnricher::generateWrapper(
    const std::string& code,
    const SourceContext& context,
    const std::string& block_id) {

    WrapperResult result;

    // Check if already a complete function
    if (isCompleteFunction(code)) {
        result.full_code = code;
        result.signature = inferSignature(code);
        result.success = true;
        return result;
    }

    // It's a snippet - generate wrapper
    FunctionSignature sig;
    sig.function_name = block_id + "_execute";
    sig.return_type = "void";  // Default for snippets

    // Generate wrapper function
    std::ostringstream wrapped;

    // Add includes from context
    for (const auto& inc : context.includes) {
        wrapped << inc << "\n";
    }
    if (!context.includes.empty()) {
        wrapped << "\n";
    }

    // Add namespaces
    for (const auto& ns : context.namespaces) {
        wrapped << "using namespace " << ns << ";\n";
    }
    if (!context.namespaces.empty()) {
        wrapped << "\n";
    }

    // Generate C-ABI wrapper function
    wrapped << "extern \"C\" {\n";
    wrapped << "\n";
    wrapped << sig.return_type << " " << sig.function_name << "() {\n";
    wrapped << "    " << code << "\n";
    wrapped << "}\n";
    wrapped << "\n";
    wrapped << "} // extern \"C\"\n";

    result.full_code = wrapped.str();
    result.signature = sig;
    result.success = true;

    return result;
}

bool BlockEnricher::isCompleteFunction(const std::string& code) {
    // Simple heuristic: complete functions have return type and parameter list
    std::regex func_pattern(R"(\w+\s+\w+\s*\([^)]*\)\s*\{)");
    return std::regex_search(code, func_pattern);
}

FunctionSignature BlockEnricher::inferSignature(const std::string& code) {
    FunctionSignature sig;

    // Try to extract function signature using regex
    std::regex func_regex(R"((\w+)\s+(\w+)\s*\(([^)]*)\))");
    std::smatch match;

    if (std::regex_search(code, match, func_regex)) {
        sig.return_type = match[1].str();
        sig.function_name = match[2].str();

        // Parse parameters
        std::string params_str = match[3].str();
        if (!params_str.empty()) {
            // Simple split by comma
            std::istringstream params_stream(params_str);
            std::string param;
            while (std::getline(params_stream, param, ',')) {
                // Trim whitespace
                param.erase(0, param.find_first_not_of(" \t"));
                param.erase(param.find_last_not_of(" \t") + 1);

                // Extract type and name
                size_t last_space = param.rfind(' ');
                if (last_space != std::string::npos) {
                    std::string type = param.substr(0, last_space);
                    std::string name = param.substr(last_space + 1);
                    sig.parameters.push_back({type, name});
                }
            }
        }
    }

    return sig;
}

// ============================================================================
// Helper Methods
// ============================================================================

std::vector<std::string> BlockEnricher::extractIncludes(
    const std::string& file_content, int line_num) {

    std::vector<std::string> includes;
    std::istringstream stream(file_content);
    std::string line;
    int current_line = 0;

    // Look at lines before the target line
    while (std::getline(stream, line) && current_line < line_num) {
        current_line++;

        // Check if it's an include directive
        if (line.find("#include") != std::string::npos) {
            includes.push_back(line);
        }
    }

    return includes;
}

std::vector<std::string> BlockEnricher::extractNamespaces(
    const std::string& file_content, int line_num) {

    std::vector<std::string> namespaces;
    std::regex ns_regex(R"(namespace\s+(\w+))");
    std::istringstream stream(file_content);
    std::string line;
    int current_line = 0;

    while (std::getline(stream, line) && current_line < line_num) {
        current_line++;

        std::smatch match;
        if (std::regex_search(line, match, ns_regex)) {
            namespaces.push_back(match[1].str());
        }
    }

    return namespaces;
}

std::string BlockEnricher::extractEnclosingClass(
    const std::string& file_content, int line_num) {

    // TODO: Implement class extraction
    // For now, return empty
    return "";
}

std::string BlockEnricher::generateWrapperFunction(
    const std::string& snippet,
    const FunctionSignature& sig,
    const SourceContext& ctx) {

    std::ostringstream wrapper;

    // Add context includes
    for (const auto& inc : ctx.includes) {
        wrapper << inc << "\n";
    }

    // Add context namespaces
    for (const auto& ns : ctx.namespaces) {
        wrapper << "using namespace " << ns << ";\n";
    }

    // Generate C-ABI function
    wrapper << "\nextern \"C\" {\n\n";
    wrapper << sig.return_type << " " << sig.function_name << "(";

    // Parameters
    for (size_t i = 0; i < sig.parameters.size(); ++i) {
        const auto& [type, name] = sig.parameters[i];
        wrapper << type << " " << name;
        if (i + 1 < sig.parameters.size()) {
            wrapper << ", ";
        }
    }

    wrapper << ") {\n";
    wrapper << "    " << snippet << "\n";
    wrapper << "}\n\n";
    wrapper << "} // extern \"C\"\n";

    return wrapper.str();
}

std::vector<std::string> BlockEnricher::detectLibraries(const std::string& code) {
    std::vector<std::string> libraries;
    std::set<std::string> detected;  // Avoid duplicates

    // Extract all #include directives
    std::regex include_pattern(R"(#include\s+[<"]([^>"]+)[>"])");
    std::sregex_iterator iter(code.begin(), code.end(), include_pattern);
    std::sregex_iterator end;

    while (iter != end) {
        std::string include_path = (*iter)[1].str();

        // Map include paths to libraries
        if (include_path.find("llvm/") == 0) {
            detected.insert("llvm");
        } else if (include_path.find("clang/") == 0) {
            detected.insert("clang");
        } else if (include_path.find("spdlog/") == 0) {
            detected.insert("spdlog");
        } else if (include_path.find("absl/") == 0 || include_path.find("abseil/") == 0) {
            detected.insert("absl");
        } else if (include_path.find("fmt/") == 0) {
            detected.insert("fmt");
        } else if (include_path == "omp.h") {
            detected.insert("openmp");
        } else if (include_path == "pthread.h" || include_path.find("pthread/") == 0) {
            detected.insert("pthread");
        } else if (include_path == "sqlite3.h") {
            detected.insert("sqlite3");
        } else if (include_path.find("openssl/") == 0) {
            detected.insert("openssl");
        } else if (include_path.find("curl/") == 0) {
            detected.insert("curl");
        } else if (include_path.find("boost/") == 0) {
            detected.insert("boost");
        } else if (include_path.find("gtest/") == 0 || include_path == "gtest.h") {
            detected.insert("gtest");
        } else if (include_path.find("gmock/") == 0) {
            detected.insert("gmock");
        } else if (include_path.find("benchmark/") == 0) {
            detected.insert("benchmark");
        } else if (include_path == "zlib.h") {
            detected.insert("zlib");
        } else if (include_path.find("protobuf/") == 0 || include_path.find("google/protobuf/") == 0) {
            detected.insert("protobuf");
        } else if (include_path.find("grpc/") == 0 || include_path.find("grpcpp/") == 0) {
            detected.insert("grpc");
        } else if (include_path.find("pybind11/") == 0) {
            detected.insert("pybind11");
        } else if (include_path.find("eigen3/") == 0 || include_path.find("Eigen/") == 0) {
            detected.insert("eigen");
        } else if (include_path.find("opencv") == 0 || include_path.find("cv.h") != std::string::npos) {
            detected.insert("opencv");
        }

        ++iter;
    }

    return std::vector<std::string>(detected.begin(), detected.end());
}

std::vector<std::string> BlockEnricher::detectIncludePaths(const std::string& code) {
    std::vector<std::string> paths;
    // This could be expanded to detect custom include paths
    // For now, return empty as standard paths are handled by library flags
    return paths;
}

} // namespace tools
} // namespace naab
