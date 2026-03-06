// NAAb Project Context Awareness — Governance as a Living Partner
//
// Implements three-layer context extraction:
//   Layer 1: LLM instruction files (CLAUDE.md, .cursorrules, etc.)
//   Layer 2: Linter/formatter configs (.editorconfig, .eslintrc, etc.)
//   Layer 3: Package manifests (package.json, go.mod, etc.)
//
// govern.json always overrides. Project context supplements only.

#include "naab/project_context.h"
#include "naab/governance.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>
#include <cctype>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace naab {
namespace governance {

namespace fs = std::filesystem;

// ============================================================================
// Language Normalization
// ============================================================================

static const std::unordered_map<std::string, std::string> LANG_ALIASES = {
    {"python", "python"}, {"py", "python"}, {"python3", "python"},
    {"javascript", "javascript"}, {"js", "javascript"}, {"node", "javascript"}, {"nodejs", "javascript"},
    {"typescript", "typescript"}, {"ts", "typescript"},
    {"go", "go"}, {"golang", "go"},
    {"rust", "rust"}, {"rs", "rust"},
    {"c++", "cpp"}, {"cpp", "cpp"}, {"cxx", "cpp"},
    {"c#", "csharp"}, {"csharp", "csharp"}, {"cs", "csharp"},
    {"shell", "shell"}, {"bash", "shell"}, {"sh", "shell"}, {"zsh", "shell"},
    {"ruby", "ruby"}, {"rb", "ruby"},
    {"php", "php"},
    {"nim", "nim"},
    {"zig", "zig"},
    {"julia", "julia"}, {"jl", "julia"},
};

std::string ProjectContextLoader::normalizeLanguage(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = LANG_ALIASES.find(lower);
    return it != LANG_ALIASES.end() ? it->second : lower;
}

// ============================================================================
// Task Normalization
// ============================================================================

static const std::unordered_map<std::string, std::string> TASK_KEYWORDS = {
    {"concurrency", "concurrency"}, {"parallel", "concurrency"}, {"async", "concurrency"},
    {"goroutine", "concurrency"}, {"thread", "concurrency"},
    {"math", "numerical_operations"}, {"numerical", "numerical_operations"},
    {"computation", "numerical_operations"}, {"calculation", "numerical_operations"},
    {"algorithm", "numerical_operations"},
    {"string", "string_processing"}, {"text", "string_processing"},
    {"parsing", "string_processing"}, {"regex", "string_processing"}, {"format", "string_processing"},
    {"file", "file_operations"}, {"filesystem", "file_operations"}, {"io", "file_operations"},
    {"directory", "file_operations"}, {"path", "file_operations"},
    {"web", "web_apis"}, {"api", "web_apis"}, {"http", "web_apis"},
    {"rest", "web_apis"}, {"fetch", "web_apis"},
    {"json", "json_processing"}, {"data", "json_processing"},
    {"serialization", "json_processing"}, {"marshal", "json_processing"},
    {"cli", "cli_tools"}, {"command", "cli_tools"}, {"shell", "cli_tools"},
    {"script", "cli_tools"}, {"tool", "cli_tools"},
    {"system", "systems_operations"}, {"memory", "systems_operations"},
    {"performance", "systems_operations"}, {"low-level", "systems_operations"},
};

std::string ProjectContextLoader::normalizeTask(const std::string& task) {
    std::string lower;
    lower.reserve(task.size());
    for (char c : task) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Try direct match first
    auto it = TASK_KEYWORDS.find(lower);
    if (it != TASK_KEYWORDS.end()) return it->second;

    // Try each keyword as substring
    for (auto& [keyword, category] : TASK_KEYWORDS) {
        if (lower.find(keyword) != std::string::npos) return category;
    }
    return "";
}

// ============================================================================
// Function-to-Language Inference
// ============================================================================

std::string ProjectContextLoader::inferLanguageFromFunction(const std::string& func_name) {
    // Known function → language mappings
    if (func_name.find("os.system") != std::string::npos ||
        func_name.find("subprocess") != std::string::npos ||
        func_name.find("pickle") != std::string::npos ||
        func_name == "exec") return "python";
    if (func_name.find("console.log") != std::string::npos ||
        func_name.find("document.") != std::string::npos ||
        func_name == "require") return "javascript";
    if (func_name.find("fmt.Println") != std::string::npos) return "go";
    if (func_name.find("println!") != std::string::npos) return "rust";
    // eval() is ambiguous — applies to python + javascript
    if (func_name == "eval") return "";  // apply to all
    return "";
}

// ============================================================================
// Rule ID Generation
// ============================================================================

std::string ProjectContextLoader::generateRuleId(
    ContextLayer layer, const std::string& file,
    const std::string& category, const std::string& key) {
    std::string prefix;
    switch (layer) {
        case ContextLayer::LLM:      prefix = "ctx-llm"; break;
        case ContextLayer::LINTER:   prefix = "ctx-lint"; break;
        case ContextLayer::MANIFEST: prefix = "ctx-manifest"; break;
    }

    // Clean the key: lowercase, replace spaces with dashes
    std::string clean_key;
    for (char c : key) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            clean_key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == ' ') {
            clean_key += '-';
        }
    }

    if (layer == ContextLayer::MANIFEST) {
        return fmt::format("{}-{}", prefix, clean_key);
    }

    // For LLM/LINTER, include a shortened file reference
    std::string short_file;
    for (char c : file) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            short_file += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (short_file.size() > 10) short_file = short_file.substr(0, 10);

    return fmt::format("{}-{}-{}", prefix, category, clean_key);
}

// ============================================================================
// Markdown Formatting Stripping
// ============================================================================

std::string ProjectContextLoader::stripMarkdownFormatting(const std::string& line) {
    std::string result = line;

    // Strip bold/italic markers
    // **text** or __text__
    static const std::regex bold_re(R"(\*\*([^*]+)\*\*)");
    result = std::regex_replace(result, bold_re, "$1");
    static const std::regex italic_re(R"(\*([^*]+)\*)");
    result = std::regex_replace(result, italic_re, "$1");

    // Strip leading bullet markers: "- ", "* ", "1. ", "2. "
    static const std::regex bullet_re(R"(^\s*[-*]\s+)");
    result = std::regex_replace(result, bullet_re, "");
    static const std::regex numbered_re(R"(^\s*\d+\.\s+)");
    result = std::regex_replace(result, numbered_re, "");

    return result;
}

// ============================================================================
// File Walking (Upward from start_dir)
// ============================================================================

std::vector<std::string> ProjectContextLoader::walkUpward(
    const std::string& start_dir,
    const std::vector<std::string>& filenames,
    const ProjectContextConfig& config) {

    std::vector<std::string> found;
    std::unordered_set<std::string> ignore_set(config.ignore_files.begin(),
                                                config.ignore_files.end());

    fs::path dir(start_dir);
    while (true) {
        for (auto& name : filenames) {
            // Handle paths with subdirectories (e.g., ".github/copilot-instructions.md")
            fs::path candidate = dir / name;
            try {
                if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
                    std::string fname = fs::path(name).filename().string();
                    // For paths like ".github/copilot-instructions.md", use the filename
                    if (name.find('/') != std::string::npos) {
                        fname = name;  // keep relative path for identification
                    }
                    if (ignore_set.count(fname)) continue;

                    // Check file size
                    auto size_kb = static_cast<int>(fs::file_size(candidate) / 1024);
                    if (config.max_file_size_kb > 0 && size_kb > config.max_file_size_kb) {
                        // Will be reported as skipped
                        continue;
                    }

                    found.push_back(candidate.string());
                }
            } catch (...) {
                // Permission errors, etc. — skip silently
            }
        }

        fs::path parent = dir.parent_path();
        if (parent == dir) break;  // Reached root
        dir = parent;
    }

    return found;
}

// ============================================================================
// Layer 1: LLM Instruction Files
// ============================================================================

static const std::vector<std::string> LLM_FILES = {
    "CLAUDE.md",
    ".cursorrules",
    ".github/copilot-instructions.md",
    "gemini.md",
    ".claude/settings.json",
    ".gemini/settings.json",
    ".aider.conf.yml",
    ".continue/config.json",
};

std::vector<ContextExtraction> ProjectContextLoader::discoverAndParseLLMFiles(
    const std::string& dir, const ProjectContextConfig& config) {

    std::vector<ContextExtraction> all;
    if (!config.sources.llm) return all;

    // Combine standard files with watch_files
    std::vector<std::string> search_files = LLM_FILES;
    for (auto& wf : config.watch_files) {
        search_files.push_back(wf);
    }

    auto found = walkUpward(dir, search_files, config);

    for (auto& path : found) {
        std::string filename = fs::path(path).filename().string();
        try {
            auto extractions = parseMarkdownFile(path, filename);
            all.insert(all.end(), extractions.begin(), extractions.end());
        } catch (...) {
            // Malformed file — skip with implicit warning
        }
    }

    return all;
}

std::vector<ContextExtraction> ProjectContextLoader::parseMarkdownFile(
    const std::string& path, const std::string& filename) {

    std::vector<ContextExtraction> results;

    std::ifstream ifs(path);
    if (!ifs.is_open()) return results;

    std::string line;
    int line_num = 0;
    bool in_code_block = false;
    std::string current_section;

    while (std::getline(ifs, line)) {
        line_num++;

        // Track code blocks (``` fences)
        if (line.find("```") != std::string::npos) {
            // Count backticks at start
            size_t backtick_count = 0;
            size_t pos = line.find_first_not_of(" \t");
            if (pos != std::string::npos) {
                while (pos + backtick_count < line.size() && line[pos + backtick_count] == '`') {
                    backtick_count++;
                }
            }
            if (backtick_count >= 3) {
                in_code_block = !in_code_block;
                continue;
            }
        }

        // Skip code block contents
        if (in_code_block) continue;

        // Skip indented code blocks (4+ spaces)
        {
            size_t indent = 0;
            while (indent < line.size() && line[indent] == ' ') indent++;
            if (indent >= 4 && !line.empty()) continue;
        }

        // Track headings for section awareness
        if (line.size() > 2 && line[0] == '#') {
            // Extract heading text
            size_t i = 0;
            while (i < line.size() && line[i] == '#') i++;
            while (i < line.size() && line[i] == ' ') i++;
            current_section = line.substr(i);
            // Lowercase for matching
            std::transform(current_section.begin(), current_section.end(),
                          current_section.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            continue;
        }

        // Skip empty lines
        if (line.find_first_not_of(" \t") == std::string::npos) continue;

        // Skip blockquotes (weaker signal)
        if (line.find_first_not_of(" \t") != std::string::npos &&
            line[line.find_first_not_of(" \t")] == '>') continue;

        // Extract directives from this line
        extractFromLine(line, line_num, filename, path, current_section, results);
    }

    return results;
}

void ProjectContextLoader::extractFromLine(
    const std::string& raw_line, int line_num,
    const std::string& source_file, const std::string& source_path,
    const std::string& section,
    std::vector<ContextExtraction>& out) {

    std::string line = stripMarkdownFormatting(raw_line);

    // Lowercase for pattern matching
    std::string lower;
    lower.reserve(line.size());
    for (char c : line) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Skip lines under sections that are primarily examples/code/setup
    // Only skip if the section NAME starts with these words (not just contains them)
    if (section.substr(0, 7) == "example" ||
        section.substr(0, 4) == "code" ||
        section.substr(0, 5) == "setup") {
        return;
    }

    // ---- Language Preference Patterns ----

    // "always use <lang>" / "only use <lang>" / "must use <lang>"
    {
        std::regex re(R"((?:always|only|must)\s+use\s+(\w+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string lang = normalizeLanguage(m[1].str());
            if (!lang.empty() && LANG_ALIASES.count(lang)) {
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LLM, source_file, "lang", "prefer-" + lang);
                ext.layer = ContextLayer::LLM;
                ext.source_file = source_file;
                ext.source_path = source_path;
                ext.line_number = line_num;
                ext.category = "language";
                ext.directive = fmt::format("strong preference for {}", lang);
                ext.mapped_rule = "optimization: all tasks +" + std::to_string(10);
                ext.original_line = raw_line;
                ext.confidence = 80;
                ext.language = lang;
                ext.score_boost = 10;
                out.push_back(ext);
            }
        }
    }

    // "prefer <lang>" / "default to <lang>"
    {
        std::regex re(R"((?:prefer|default\s+to)\s+(\w+?)(?:\s+for\s+(.+?))?(?:\.|,|$))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string lang = normalizeLanguage(m[1].str());
            std::string task_str = m[2].matched ? m[2].str() : "";
            if (!lang.empty() && LANG_ALIASES.count(lang)) {
                ContextExtraction ext;
                ext.layer = ContextLayer::LLM;
                ext.source_file = source_file;
                ext.source_path = source_path;
                ext.line_number = line_num;
                ext.category = "language";
                ext.original_line = raw_line;
                ext.confidence = 80;
                ext.language = lang;

                if (!task_str.empty()) {
                    ext.task = normalizeTask(task_str);
                    if (!ext.task.empty()) {
                        ext.id = generateRuleId(ContextLayer::LLM, source_file, "lang",
                                                "task-" + lang + "-" + ext.task);
                        ext.directive = fmt::format("prefer {} for {}", lang, ext.task);
                        ext.mapped_rule = fmt::format("optimization:{} +15", ext.task);
                        ext.score_boost = 15;
                    } else {
                        ext.id = generateRuleId(ContextLayer::LLM, source_file, "lang", "prefer-" + lang);
                        ext.directive = fmt::format("prefer {}", lang);
                        ext.mapped_rule = "optimization: all tasks +10";
                        ext.score_boost = 10;
                    }
                } else {
                    ext.id = generateRuleId(ContextLayer::LLM, source_file, "lang", "prefer-" + lang);
                    ext.directive = fmt::format("prefer {}", lang);
                    ext.mapped_rule = "optimization: all tasks +10";
                    ext.score_boost = 10;
                }
                out.push_back(ext);
            }
        }
    }

    // "use <lang> for <task>"
    {
        std::regex re(R"(use\s+(\w+)\s+(?:for|when)\s+(.+?)(?:\.|,|$))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string lang = normalizeLanguage(m[1].str());
            std::string task_str = m[2].str();
            if (!lang.empty() && LANG_ALIASES.count(lang)) {
                std::string task = normalizeTask(task_str);
                if (!task.empty()) {
                    ContextExtraction ext;
                    ext.id = generateRuleId(ContextLayer::LLM, source_file, "lang",
                                            "task-" + lang + "-" + task);
                    ext.layer = ContextLayer::LLM;
                    ext.source_file = source_file;
                    ext.source_path = source_path;
                    ext.line_number = line_num;
                    ext.category = "language";
                    ext.directive = fmt::format("use {} for {}", lang, task);
                    ext.mapped_rule = fmt::format("optimization:{} +15", task);
                    ext.original_line = raw_line;
                    ext.confidence = 80;
                    ext.language = lang;
                    ext.task = task;
                    ext.score_boost = 15;
                    out.push_back(ext);
                }
            }
        }
    }

    // "never use <lang>" / "avoid <lang>" / "don't use <lang>"
    {
        std::regex re(R"((?:never|don'?t|do\s+not|avoid)\s+use\s+(\w+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string lang = normalizeLanguage(m[1].str());
            if (!lang.empty() && LANG_ALIASES.count(lang)) {
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LLM, source_file, "lang", "avoid-" + lang);
                ext.layer = ContextLayer::LLM;
                ext.source_file = source_file;
                ext.source_path = source_path;
                ext.line_number = line_num;
                ext.category = "language";
                ext.directive = fmt::format("avoid {}", lang);
                ext.mapped_rule = "languages.blocked (advisory)";
                ext.original_line = raw_line;
                ext.confidence = 80;
                ext.language = lang;
                ext.score_boost = -10;
                out.push_back(ext);
            }
        }
    }

    // ---- Banned Pattern Patterns ----

    // "never use <func>()" / "don't use <func>" / "avoid <func>"
    {
        std::regex re(R"((?:never|don'?t|do\s+not|avoid)\s+(?:use|call)\s+([\w.]+)\s*\(?)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string func = m[1].str();
            // Don't re-match language avoidance patterns
            std::string func_lower = func;
            std::transform(func_lower.begin(), func_lower.end(), func_lower.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            if (LANG_ALIASES.find(func_lower) == LANG_ALIASES.end()) {
                std::string inferred_lang = inferLanguageFromFunction(func);
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LLM, source_file, "ban", func_lower);
                ext.layer = ContextLayer::LLM;
                ext.source_file = source_file;
                ext.source_path = source_path;
                ext.line_number = line_num;
                ext.category = "banned";
                ext.directive = fmt::format("ban {}", func);
                ext.mapped_rule = inferred_lang.empty()
                    ? "*.banned_functions"
                    : fmt::format("{}.banned_functions", inferred_lang);
                ext.original_line = raw_line;
                ext.confidence = 80;
                ext.language = inferred_lang;
                out.push_back(ext);
            }
        }
    }

    // "no <module>" / "ban <module>" / "forbid <module>"
    {
        std::regex re(R"((?:no|ban|forbid|prohibit)\s+([\w.]+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string mod = m[1].str();
            std::string mod_lower = mod;
            std::transform(mod_lower.begin(), mod_lower.end(), mod_lower.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            // Skip if it's a known language name (handled by language patterns)
            if (LANG_ALIASES.find(mod_lower) == LANG_ALIASES.end() &&
                mod_lower != "console" && mod_lower.find('.') != std::string::npos) {
                std::string inferred_lang = inferLanguageFromFunction(mod);
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LLM, source_file, "ban", mod_lower);
                ext.layer = ContextLayer::LLM;
                ext.source_file = source_file;
                ext.source_path = source_path;
                ext.line_number = line_num;
                ext.category = "banned";
                ext.directive = fmt::format("ban import {}", mod);
                ext.mapped_rule = inferred_lang.empty()
                    ? "*.banned_imports"
                    : fmt::format("{}.banned_imports", inferred_lang);
                ext.original_line = raw_line;
                ext.confidence = 80;
                ext.language = inferred_lang;
                out.push_back(ext);
            }
        }
    }

    // "never import <module>"
    {
        std::regex re(R"((?:never|don'?t|do\s+not)\s+import\s+([\w.]+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string mod = m[1].str();
            std::string mod_lower = mod;
            std::transform(mod_lower.begin(), mod_lower.end(), mod_lower.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            std::string inferred_lang = inferLanguageFromFunction(mod);
            ContextExtraction ext;
            ext.id = generateRuleId(ContextLayer::LLM, source_file, "ban", "import-" + mod_lower);
            ext.layer = ContextLayer::LLM;
            ext.source_file = source_file;
            ext.source_path = source_path;
            ext.line_number = line_num;
            ext.category = "banned";
            ext.directive = fmt::format("ban import {}", mod);
            ext.mapped_rule = inferred_lang.empty()
                ? "*.banned_imports"
                : fmt::format("{}.banned_imports", inferred_lang);
            ext.original_line = raw_line;
            ext.confidence = 80;
            ext.language = inferred_lang;
            out.push_back(ext);
        }
    }

    // ---- Style Patterns ----

    // "use N spaces"
    {
        std::regex re(R"(use\s+(\d+)\s+spaces?(?:\s+(?:for\s+)?indent)?)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            int n = std::stoi(m[1].str());
            ContextExtraction ext;
            ext.id = generateRuleId(ContextLayer::LLM, source_file, "style", fmt::format("indent-{}", n));
            ext.layer = ContextLayer::LLM;
            ext.source_file = source_file;
            ext.source_path = source_path;
            ext.line_number = line_num;
            ext.category = "style";
            ext.directive = fmt::format("indent_size={}, indent_style=spaces", n);
            ext.mapped_rule = fmt::format("indent_size={}", n);
            ext.original_line = raw_line;
            ext.confidence = 80;
            out.push_back(ext);
        }
    }

    // "use tabs"
    {
        std::regex re(R"((?:use|prefer)\s+tabs?\s*(?:for\s+indent)?)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            ContextExtraction ext;
            ext.id = generateRuleId(ContextLayer::LLM, source_file, "style", "tabs");
            ext.layer = ContextLayer::LLM;
            ext.source_file = source_file;
            ext.source_path = source_path;
            ext.line_number = line_num;
            ext.category = "style";
            ext.directive = "indent_style=tabs";
            ext.mapped_rule = "indent_style=tabs";
            ext.original_line = raw_line;
            ext.confidence = 80;
            out.push_back(ext);
        }
    }

    // "no console.log"
    {
        if (lower.find("no console.log") != std::string::npos ||
            lower.find("remove console.log") != std::string::npos ||
            lower.find("ban console.log") != std::string::npos) {
            ContextExtraction ext;
            ext.id = generateRuleId(ContextLayer::LLM, source_file, "style", "no-console-log");
            ext.layer = ContextLayer::LLM;
            ext.source_file = source_file;
            ext.source_path = source_path;
            ext.line_number = line_num;
            ext.category = "style";
            ext.directive = "no_console_log=true";
            ext.mapped_rule = "javascript.no_console_log";
            ext.original_line = raw_line;
            ext.confidence = 80;
            ext.language = "javascript";
            out.push_back(ext);
        }
    }

    // "use strict mode"
    {
        std::regex re(R"((?:use|require|enable)\s+strict\s+mode)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            ContextExtraction ext;
            ext.id = generateRuleId(ContextLayer::LLM, source_file, "style", "strict-mode");
            ext.layer = ContextLayer::LLM;
            ext.source_file = source_file;
            ext.source_path = source_path;
            ext.line_number = line_num;
            ext.category = "style";
            ext.directive = "strict_mode=true";
            ext.mapped_rule = "javascript.strict_mode";
            ext.original_line = raw_line;
            ext.confidence = 80;
            ext.language = "javascript";
            out.push_back(ext);
        }
    }

    // "max N lines per function"
    {
        std::regex re(R"((?:max(?:imum)?|limit)\s+(\d+)\s+lines?\s+(?:per|in)\s+(?:function|method))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            int n = std::stoi(m[1].str());
            ContextExtraction ext;
            ext.id = generateRuleId(ContextLayer::LLM, source_file, "style", fmt::format("max-func-lines-{}", n));
            ext.layer = ContextLayer::LLM;
            ext.source_file = source_file;
            ext.source_path = source_path;
            ext.line_number = line_num;
            ext.category = "style";
            ext.directive = fmt::format("max_function_length={}", n);
            ext.mapped_rule = fmt::format("complexity.max_lines_per_block={}", n);
            ext.original_line = raw_line;
            ext.confidence = 80;
            out.push_back(ext);
        }
    }
}

// ============================================================================
// Layer 2: Linter/Formatter Configs
// ============================================================================

static const std::vector<std::string> LINTER_FILES = {
    ".editorconfig",
    ".eslintrc.json",
    ".eslintrc.yml",
    ".eslintrc",
    ".prettierrc",
    ".prettierrc.json",
    ".clang-format",
    "rustfmt.toml",
    ".rubocop.yml",
    "biome.json",
};

std::vector<ContextExtraction> ProjectContextLoader::discoverAndParseLinterConfigs(
    const std::string& dir, const ProjectContextConfig& config) {

    std::vector<ContextExtraction> all;
    if (!config.sources.linters) return all;

    auto found = walkUpward(dir, LINTER_FILES, config);

    for (auto& path : found) {
        std::string filename = fs::path(path).filename().string();
        try {
            if (filename == ".editorconfig") {
                auto exts = parseEditorConfig(path);
                all.insert(all.end(), exts.begin(), exts.end());
            } else if (filename.find(".eslintrc") != std::string::npos ||
                       filename == ".prettierrc" || filename == ".prettierrc.json" ||
                       filename == "biome.json") {
                auto exts = parseJsonConfig(path, filename);
                all.insert(all.end(), exts.begin(), exts.end());
            }
            // .clang-format, rustfmt.toml, .rubocop.yml would need YAML/TOML parsers
            // For now, only parse structured JSON and INI-like (.editorconfig) files
        } catch (...) {
            // Malformed config — skip
        }
    }

    return all;
}

std::vector<ContextExtraction> ProjectContextLoader::parseEditorConfig(const std::string& path) {
    std::vector<ContextExtraction> results;
    std::string filename = ".editorconfig";

    std::ifstream ifs(path);
    if (!ifs.is_open()) return results;

    std::string line;
    while (std::getline(ifs, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip comments and sections
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;

        // Parse key=value
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val = val.substr(1);

        // Lowercase key
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        ContextExtraction ext;
        ext.layer = ContextLayer::LINTER;
        ext.source_file = filename;
        ext.source_path = path;
        ext.confidence = 100;

        if (key == "indent_style") {
            ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "indent-style");
            ext.category = "style";
            ext.directive = fmt::format("indent_style={}", val == "tab" ? "tabs" : "spaces");
            ext.mapped_rule = fmt::format("indent_style={}", val == "tab" ? "tabs" : "spaces");
            ext.original_line = line;
            results.push_back(ext);
        } else if (key == "indent_size") {
            ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "indent-size");
            ext.category = "style";
            ext.directive = fmt::format("indent_size={}", val);
            ext.mapped_rule = fmt::format("indent_size={}", val);
            ext.original_line = line;
            results.push_back(ext);
        } else if (key == "max_line_length") {
            ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "max-line-length");
            ext.category = "style";
            ext.directive = fmt::format("max_line_length={}", val);
            ext.mapped_rule = fmt::format("complexity.max_line_length={}", val);
            ext.original_line = line;
            results.push_back(ext);
        }
    }

    return results;
}

std::vector<ContextExtraction> ProjectContextLoader::parseJsonConfig(
    const std::string& path, const std::string& filename) {

    std::vector<ContextExtraction> results;

    std::ifstream ifs(path);
    if (!ifs.is_open()) return results;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ifs);
    } catch (...) {
        return results;  // Malformed JSON — skip
    }

    // ESLint
    if (filename.find("eslintrc") != std::string::npos) {
        if (j.contains("rules") && j["rules"].is_object()) {
            auto& rules = j["rules"];
            if (rules.contains("no-eval")) {
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LINTER, filename, "ban", "no-eval");
                ext.layer = ContextLayer::LINTER;
                ext.source_file = filename;
                ext.source_path = path;
                ext.category = "banned";
                ext.directive = "ban eval()";
                ext.mapped_rule = "javascript.banned_functions: [eval]";
                ext.original_line = "no-eval";
                ext.confidence = 100;
                ext.language = "javascript";
                results.push_back(ext);
            }
            if (rules.contains("no-console")) {
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "no-console");
                ext.layer = ContextLayer::LINTER;
                ext.source_file = filename;
                ext.source_path = path;
                ext.category = "style";
                ext.directive = "no_console_log=true";
                ext.mapped_rule = "javascript.no_console_log";
                ext.original_line = "no-console";
                ext.confidence = 100;
                ext.language = "javascript";
                results.push_back(ext);
            }
            if (rules.contains("strict")) {
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "strict");
                ext.layer = ContextLayer::LINTER;
                ext.source_file = filename;
                ext.source_path = path;
                ext.category = "style";
                ext.directive = "strict_mode=true";
                ext.mapped_rule = "javascript.strict_mode";
                ext.original_line = "strict";
                ext.confidence = 100;
                ext.language = "javascript";
                results.push_back(ext);
            }
        }
    }

    // Prettier
    if (filename.find("prettierrc") != std::string::npos) {
        if (j.contains("tabWidth")) {
            ContextExtraction ext;
            ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "tabwidth");
            ext.layer = ContextLayer::LINTER;
            ext.source_file = filename;
            ext.source_path = path;
            ext.category = "style";
            int tw = j["tabWidth"].get<int>();
            ext.directive = fmt::format("indent_size={}", tw);
            ext.mapped_rule = fmt::format("indent_size={}", tw);
            ext.original_line = fmt::format("tabWidth: {}", tw);
            ext.confidence = 100;
            results.push_back(ext);
        }
        if (j.contains("useTabs")) {
            ContextExtraction ext;
            ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "usetabs");
            ext.layer = ContextLayer::LINTER;
            ext.source_file = filename;
            ext.source_path = path;
            ext.category = "style";
            bool tabs = j["useTabs"].get<bool>();
            ext.directive = fmt::format("indent_style={}", tabs ? "tabs" : "spaces");
            ext.mapped_rule = fmt::format("indent_style={}", tabs ? "tabs" : "spaces");
            ext.original_line = fmt::format("useTabs: {}", tabs ? "true" : "false");
            ext.confidence = 100;
            results.push_back(ext);
        }
    }

    // Biome
    if (filename == "biome.json") {
        if (j.contains("formatter") && j["formatter"].is_object()) {
            auto& fmt_cfg = j["formatter"];
            if (fmt_cfg.contains("indentWidth")) {
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "indent-width");
                ext.layer = ContextLayer::LINTER;
                ext.source_file = filename;
                ext.source_path = path;
                ext.category = "style";
                int iw = fmt_cfg["indentWidth"].get<int>();
                ext.directive = fmt::format("indent_size={}", iw);
                ext.mapped_rule = fmt::format("indent_size={}", iw);
                ext.original_line = fmt::format("indentWidth: {}", iw);
                ext.confidence = 100;
                results.push_back(ext);
            }
            if (fmt_cfg.contains("indentStyle")) {
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::LINTER, filename, "style", "indent-style-biome");
                ext.layer = ContextLayer::LINTER;
                ext.source_file = filename;
                ext.source_path = path;
                ext.category = "style";
                std::string style = fmt_cfg["indentStyle"].get<std::string>();
                ext.directive = fmt::format("indent_style={}", style);
                ext.mapped_rule = fmt::format("indent_style={}", style);
                ext.original_line = fmt::format("indentStyle: {}", style);
                ext.confidence = 100;
                results.push_back(ext);
            }
        }
    }

    return results;
}

// ============================================================================
// Layer 3: Package Manifests
// ============================================================================

struct ManifestSpec {
    std::string filename;
    std::string language;
};

static const std::vector<ManifestSpec> MANIFEST_FILES = {
    {"package.json", "javascript"},
    {"go.mod", "go"},
    {"Cargo.toml", "rust"},
    {"pyproject.toml", "python"},
    {"Gemfile", "ruby"},
    {"composer.json", "php"},
    {"mix.exs", "elixir"},
    {"Project.toml", "julia"},
};

std::vector<ContextExtraction> ProjectContextLoader::discoverAndParseManifests(
    const std::string& dir, const ProjectContextConfig& config) {

    std::vector<ContextExtraction> all;
    if (!config.sources.manifests) return all;

    std::vector<std::string> filenames;
    for (auto& spec : MANIFEST_FILES) {
        filenames.push_back(spec.filename);
    }

    auto found = walkUpward(dir, filenames, config);

    for (auto& path : found) {
        std::string filename = fs::path(path).filename().string();
        // Find language for this manifest
        std::string lang;
        for (auto& spec : MANIFEST_FILES) {
            if (spec.filename == filename) {
                lang = spec.language;
                break;
            }
        }
        if (!lang.empty()) {
            auto exts = parseManifestFile(path, filename, lang);
            all.insert(all.end(), exts.begin(), exts.end());
        }
    }

    // Also check for .nimble files
    try {
        fs::path nimble_dir(dir);
        for (auto& entry : fs::directory_iterator(nimble_dir)) {
            if (entry.path().extension() == ".nimble") {
                ContextExtraction ext;
                ext.id = generateRuleId(ContextLayer::MANIFEST, "", "manifest", "nim");
                ext.layer = ContextLayer::MANIFEST;
                ext.source_file = entry.path().filename().string();
                ext.source_path = entry.path().string();
                ext.category = "manifest";
                ext.directive = "Nim detected";
                ext.mapped_rule = "optimization: nim +5";
                ext.confidence = 60;
                ext.language = "nim";
                ext.score_boost = 5;
                all.push_back(ext);
                break;
            }
        }
    } catch (...) {
        // Directory iteration failed — skip
    }

    return all;
}

std::vector<ContextExtraction> ProjectContextLoader::parseManifestFile(
    const std::string& path, const std::string& filename,
    const std::string& language) {

    std::vector<ContextExtraction> results;

    ContextExtraction ext;
    ext.id = generateRuleId(ContextLayer::MANIFEST, "", "manifest", language);
    ext.layer = ContextLayer::MANIFEST;
    ext.source_file = filename;
    ext.source_path = path;
    ext.category = "manifest";
    ext.confidence = 60;
    ext.language = language;
    ext.score_boost = 5;

    // Try to extract version info
    std::string version_info;
    if (filename == "package.json") {
        try {
            std::ifstream ifs(path);
            auto j = nlohmann::json::parse(ifs);
            if (j.contains("engines") && j["engines"].is_object() &&
                j["engines"].contains("node")) {
                version_info = fmt::format("node {}", j["engines"]["node"].get<std::string>());
            }
        } catch (...) {}
    } else if (filename == "go.mod") {
        try {
            std::ifstream ifs(path);
            std::string line;
            while (std::getline(ifs, line)) {
                if (line.substr(0, 3) == "go ") {
                    version_info = fmt::format("Go {}", line.substr(3));
                    break;
                }
            }
        } catch (...) {}
    }

    // Create the detection extraction
    ext.directive = fmt::format("{} detected{}", language,
                                version_info.empty() ? "" : fmt::format(" ({})", version_info));
    ext.mapped_rule = fmt::format("optimization: {} +5", language);
    results.push_back(ext);

    // If version info available, add version metadata extraction
    if (!version_info.empty()) {
        ContextExtraction ver_ext;
        ver_ext.id = generateRuleId(ContextLayer::MANIFEST, "", "manifest", language + "-version");
        ver_ext.layer = ContextLayer::MANIFEST;
        ver_ext.source_file = filename;
        ver_ext.source_path = path;
        ver_ext.category = "manifest";
        ver_ext.directive = fmt::format("{} version: {}", language, version_info);
        ver_ext.mapped_rule = "informational";
        ver_ext.confidence = 60;
        ver_ext.language = language;
        results.push_back(ver_ext);
    }

    return results;
}

// ============================================================================
// Conflict Resolution
// ============================================================================

void ProjectContextLoader::resolveConflicts(
    std::vector<ContextExtraction>& extractions,
    const ProjectContextConfig& config) {

    // Track seen rule IDs to detect conflicts
    std::unordered_map<std::string, size_t> seen_ids;  // id -> index

    for (size_t i = 0; i < extractions.size(); i++) {
        auto& ext = extractions[i];

        // Check suppress_rules
        for (auto& suppressed : config.suppress_rules) {
            if (ext.id == suppressed) {
                ext.status = "skipped:suppressed";
                ext.skip_reason = fmt::format("Suppressed by suppress_rules: {}", suppressed);
                break;
            }
        }
        if (!ext.status.empty()) continue;

        // Deduplicate by rule ID — if same ID already seen, mark as redundant
        {
            auto id_it = seen_ids.find("id:" + ext.id);
            if (id_it != seen_ids.end()) {
                ext.status = "redundant";
                ext.skip_reason = fmt::format("Duplicate of {} ({}:{})",
                    extractions[id_it->second].source_file,
                    extractions[id_it->second].source_file,
                    extractions[id_it->second].line_number);
                continue;
            }
            seen_ids["id:" + ext.id] = i;
        }

        // For language preferences with the same language but from different files
        // (conflict detection within same category)
        std::string conflict_key;
        if (ext.category == "language" && !ext.language.empty()) {
            conflict_key = "lang-" + ext.language;
            if (!ext.task.empty()) conflict_key += "-" + ext.task;
        } else if (ext.category == "style") {
            // Use mapped_rule as conflict key for style rules
            auto eq = ext.mapped_rule.find('=');
            if (eq != std::string::npos) {
                conflict_key = "style-" + ext.mapped_rule.substr(0, eq);
            }
        } else if (ext.category == "banned") {
            // Deduplicate bans with same directive across files
            conflict_key = "ban-" + ext.directive;
        }

        if (!conflict_key.empty()) {
            auto it = seen_ids.find(conflict_key);
            if (it != seen_ids.end()) {
                auto& existing = extractions[it->second];
                // Determine winner
                bool existing_wins = true;
                if (!config.priority_source.empty()) {
                    // priority_source wins
                    if (ext.source_file == config.priority_source &&
                        existing.source_file != config.priority_source) {
                        existing_wins = false;
                    }
                } else {
                    // Layer priority: LINTER > LLM > MANIFEST
                    if (ext.layer < existing.layer) {
                        existing_wins = false;
                    }
                    // Within same layer, first-file-wins (existing was found first)
                }

                if (existing_wins) {
                    ext.status = "skipped:conflict";
                    ext.skip_reason = fmt::format("Conflicts with {} ({}:{})",
                        existing.source_file, existing.source_file, existing.line_number);
                } else {
                    existing.status = "skipped:conflict";
                    existing.skip_reason = fmt::format("Overridden by {} (priority_source)",
                        ext.source_file);
                    seen_ids[conflict_key] = i;
                }
            } else {
                seen_ids[conflict_key] = i;
            }
        }
    }
}

// ============================================================================
// Main Entry Point
// ============================================================================

std::vector<ContextExtraction> ProjectContextLoader::loadContext(
    const std::string& project_dir,
    const ProjectContextConfig& config) {

    std::vector<ContextExtraction> all;

    if (!config.enabled) return all;

    // Layer 1: LLM files
    auto llm = discoverAndParseLLMFiles(project_dir, config);

    // Filter by extract categories
    for (auto& ext : llm) {
        if (ext.category == "language" && !config.extract_language_prefs) continue;
        if (ext.category == "banned" && !config.extract_banned_patterns) continue;
        if (ext.category == "style" && !config.extract_style_rules) continue;
        if (ext.category == "custom" && !config.extract_custom_directives) continue;
        all.push_back(std::move(ext));
    }

    // Layer 2: Linter configs
    auto linter = discoverAndParseLinterConfigs(project_dir, config);
    for (auto& ext : linter) {
        if (ext.category == "style" && !config.extract_style_rules) continue;
        if (ext.category == "banned" && !config.extract_banned_patterns) continue;
        all.push_back(std::move(ext));
    }

    // Layer 3: Manifests
    auto manifest = discoverAndParseManifests(project_dir, config);
    all.insert(all.end(), manifest.begin(), manifest.end());

    // Resolve conflicts
    resolveConflicts(all, config);

    return all;
}

// ============================================================================
// Apply to Rules
// ============================================================================

void ProjectContextLoader::applyToRules(
    GovernanceRules& rules,
    std::vector<ContextExtraction>& extractions,
    EnforcementLevel /*level*/) {

    for (auto& ext : extractions) {
        // Skip already-handled extractions
        if (!ext.status.empty()) continue;

        // Informational extractions (manifest version info)
        if (ext.mapped_rule == "informational") {
            ext.status = "applied";
            continue;
        }

        // Check govern.json overrides
        if (ext.category == "language") {
            // If govern.json explicitly sets allowed_languages, skip LLM preferences
            if (!rules.allowed_languages.empty() || !rules.languages.allowed.empty()) {
                if (ext.score_boost > 0) {
                    // Check if this language is even allowed
                    bool is_allowed = rules.allowed_languages.empty() && rules.languages.allowed.empty();
                    if (!is_allowed) {
                        is_allowed = rules.allowed_languages.count(ext.language) > 0 ||
                                     rules.languages.allowed.count(ext.language) > 0;
                    }
                    if (!is_allowed) {
                        ext.status = "skipped:govern";
                        ext.skip_reason = "govern.json restricts languages";
                        continue;
                    }
                }
            }
            // Negative preference (avoid) — advisory only, don't block
            ext.status = "applied";
            continue;
        }

        if (ext.category == "banned") {
            // Additive — always safe to apply
            // Check if already in govern.json
            std::string func_lower = ext.directive;
            if (func_lower.substr(0, 4) == "ban ") func_lower = func_lower.substr(4);

            bool already_banned = false;
            // Check per-language banned lists
            if (!ext.language.empty()) {
                auto it = rules.languages.per_language.find(ext.language);
                if (it != rules.languages.per_language.end()) {
                    for (auto& bf : it->second.banned_functions) {
                        if (bf == func_lower) { already_banned = true; break; }
                    }
                    if (!already_banned) {
                        for (auto& bi : it->second.banned_imports) {
                            if (bi == func_lower) { already_banned = true; break; }
                        }
                    }
                }
            }

            if (already_banned) {
                ext.status = "redundant";
                ext.skip_reason = "Already in govern.json";
            } else {
                // Apply: add to per-language banned list
                if (!ext.language.empty()) {
                    if (ext.mapped_rule.find("banned_imports") != std::string::npos) {
                        rules.languages.per_language[ext.language].banned_imports.push_back(func_lower);
                    } else {
                        rules.languages.per_language[ext.language].banned_functions.push_back(func_lower);
                    }
                }
                ext.status = "applied";
            }
            continue;
        }

        if (ext.category == "style") {
            // Override-sensitive — check if govern.json already set it
            if (ext.mapped_rule.find("indent_size=") != std::string::npos) {
                // Check if any per-language config already sets indent_size
                bool gov_set = false;
                for (auto& [lang, cfg] : rules.languages.per_language) {
                    if (cfg.indent_size > 0) { gov_set = true; break; }
                }
                if (gov_set) {
                    ext.status = "skipped:govern";
                    ext.skip_reason = "govern.json sets indent_size";
                    continue;
                }
            }
            if (ext.mapped_rule.find("indent_style=") != std::string::npos) {
                bool gov_set = false;
                for (auto& [lang, cfg] : rules.languages.per_language) {
                    if (!cfg.indent_style.empty()) { gov_set = true; break; }
                }
                if (gov_set) {
                    ext.status = "skipped:govern";
                    ext.skip_reason = "govern.json sets indent_style";
                    continue;
                }
            }
            if (ext.mapped_rule.find("no_console_log") != std::string::npos) {
                auto it = rules.languages.per_language.find("javascript");
                if (it != rules.languages.per_language.end() && it->second.no_console_log) {
                    ext.status = "redundant";
                    ext.skip_reason = "Already in govern.json";
                    continue;
                }
                rules.languages.per_language["javascript"].no_console_log = true;
            }
            if (ext.mapped_rule.find("strict_mode") != std::string::npos) {
                auto it = rules.languages.per_language.find("javascript");
                if (it != rules.languages.per_language.end() && it->second.strict_mode) {
                    ext.status = "redundant";
                    ext.skip_reason = "Already in govern.json";
                    continue;
                }
                rules.languages.per_language["javascript"].strict_mode = true;
            }
            ext.status = "applied";
            continue;
        }

        if (ext.category == "manifest") {
            // Manifests are always ADVISORY — just mark as applied for reporting
            ext.status = "applied";
            continue;
        }

        // Default: apply
        ext.status = "applied";
    }
}

// ============================================================================
// Optimization Hints
// ============================================================================

void ProjectContextLoader::applyOptimizationHints(
    PolyglotOptimizationConfig& opt_config,
    const std::vector<ContextExtraction>& extractions) {

    for (auto& ext : extractions) {
        if (ext.status != "applied") continue;
        if (ext.score_boost == 0) continue;
        if (ext.language.empty()) continue;

        if (!ext.task.empty()) {
            // Task-specific boost
            auto& score = opt_config.task_language_matrix[ext.task][ext.language];
            score.score = std::min(100, score.score + ext.score_boost);
            if (score.reason.empty()) {
                score.reason = fmt::format("project context: {}", ext.directive);
            }
        } else {
            // All-task boost
            static const std::vector<std::string> all_tasks = {
                "numerical_operations", "string_processing", "file_operations",
                "systems_operations", "web_apis", "concurrency",
                "json_processing", "cli_tools"
            };
            for (auto& task : all_tasks) {
                auto& score = opt_config.task_language_matrix[task][ext.language];
                score.score = std::min(100, score.score + ext.score_boost);
            }
        }
    }
}

// ============================================================================
// Report Formatting
// ============================================================================

std::string ProjectContextLoader::formatReport(
    const std::vector<ContextExtraction>& extractions,
    bool verbose) {

    if (extractions.empty()) return "";

    std::string out;
    bool is_dry_run = false;

    // Check if any extraction is dry_run
    for (auto& ext : extractions) {
        if (ext.status == "dry_run") { is_dry_run = true; break; }
    }

    if (is_dry_run) {
        out += "[project-context] DRY RUN - previewing extractions (no rules applied)\n";
    }

    // Group by layer
    std::vector<const ContextExtraction*> llm_exts, linter_exts, manifest_exts;
    for (auto& ext : extractions) {
        switch (ext.layer) {
            case ContextLayer::LLM:      llm_exts.push_back(&ext); break;
            case ContextLayer::LINTER:   linter_exts.push_back(&ext); break;
            case ContextLayer::MANIFEST: manifest_exts.push_back(&ext); break;
        }
    }

    auto format_status = [](const std::string& status) -> std::string {
        if (status == "applied") return "[APPLIED]";
        if (status == "skipped:conflict") return "[SKIPPED: conflict]";
        if (status == "skipped:govern") return "[SKIPPED: govern.json]";
        if (status == "skipped:suppressed") return "[SKIPPED: suppressed]";
        if (status == "redundant") return "[REDUNDANT]";
        if (status == "dry_run") return "[DRY RUN]";
        return "[" + status + "]";
    };

    auto format_category = [](const std::string& cat) -> std::string {
        if (cat == "language") return "Language";
        if (cat == "banned") return "Banned";
        if (cat == "style") return "Style";
        if (cat == "manifest") return "Manifest";
        if (cat == "custom") return "Custom";
        return cat;
    };

    // Layer 1
    if (!llm_exts.empty()) {
        out += "[project-context] Layer 1: LLM Instruction Files\n";
        // Group by source file
        std::string current_file;
        int file_count = 0;
        int max_display = verbose ? 1000 : 10;

        for (auto* ext : llm_exts) {
            if (ext->source_file != current_file) {
                current_file = ext->source_file;
                file_count = 0;
                // Count directives for this file
                int total = 0;
                for (auto* e : llm_exts) {
                    if (e->source_file == current_file) total++;
                }
                out += fmt::format("  {} ({} directives)\n", current_file, total);
            }
            file_count++;
            if (file_count <= max_display) {
                out += fmt::format("  {}{} {:8s} \"{}\" {} {}\n",
                    file_count == 1 ? "\xe2\x94\x9c\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ",  // ├─
                    "",
                    format_category(ext->category) + ":",
                    ext->directive,
                    ext->mapped_rule.empty() ? "" : "\xe2\x86\x92 " + ext->mapped_rule,  // →
                    format_status(ext->status));
            }
        }
        if (!verbose && file_count > max_display) {
            out += fmt::format("  ... and {} more (use --verbose for full list)\n",
                               file_count - max_display);
        }
    }

    // Layer 2
    if (!linter_exts.empty()) {
        out += "[project-context] Layer 2: Linter/Formatter Configs\n";
        std::string current_file;
        for (auto* ext : linter_exts) {
            if (ext->source_file != current_file) {
                current_file = ext->source_file;
                int total = 0;
                for (auto* e : linter_exts) {
                    if (e->source_file == current_file) total++;
                }
                out += fmt::format("  {} ({} rules)\n", current_file, total);
            }
            out += fmt::format("  \xe2\x94\x9c\xe2\x94\x80 {:8s} {} {} {}\n",
                format_category(ext->category) + ":",
                ext->directive,
                ext->mapped_rule.empty() ? "" : "\xe2\x86\x92 " + ext->mapped_rule,
                format_status(ext->status));
        }
    }

    // Layer 3
    if (!manifest_exts.empty()) {
        out += "[project-context] Layer 3: Package Manifests\n";
        for (auto* ext : manifest_exts) {
            if (ext->category == "manifest" && ext->mapped_rule != "informational") {
                out += fmt::format("  {} \xe2\x86\x92 {} [optimization +{}]\n",
                    ext->source_file, ext->directive, ext->score_boost);
            }
        }
    }

    // Summary
    int applied = 0, skipped = 0, suppressed = 0, redundant = 0;
    for (auto& ext : extractions) {
        if (ext.status == "applied") applied++;
        else if (ext.status.find("skipped") != std::string::npos) skipped++;
        else if (ext.status == "redundant") redundant++;
    }
    // Count suppressed separately from other skips
    for (auto& ext : extractions) {
        if (ext.status == "skipped:suppressed") { suppressed++; skipped--; }
    }

    out += fmt::format("[project-context] Summary: {} extracted, {} applied, {} skipped, {} suppressed, {} redundant\n",
        extractions.size(), applied, skipped, suppressed, redundant);

    if (is_dry_run) {
        out += "[project-context] DRY RUN COMPLETE - set \"dry_run\": false to apply\n";
    }

    return out;
}

} // namespace governance
} // namespace naab
