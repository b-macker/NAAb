#include "style_config.h"
#include "formatter.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace naab {
namespace formatter {

// ============================================================================
// TOML Parser Implementation
// ============================================================================

TomlParser::TomlParser(const std::string& content) : content_(content) {}

std::string TomlParser::trim(const std::string& str) const {
    size_t start = 0;
    size_t end = str.length();

    while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }

    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }

    return str.substr(start, end - start);
}

void TomlParser::parseLine(const std::string& line, std::string& current_section) {
    std::string trimmed = trim(line);

    // Skip empty lines and comments
    if (trimmed.empty() || trimmed[0] == '#') {
        return;
    }

    // Section header: [section]
    if (trimmed[0] == '[' && trimmed.back() == ']') {
        current_section = trimmed.substr(1, trimmed.length() - 2);
        current_section = trim(current_section);
        return;
    }

    // Key-value pair: key = value
    size_t equals_pos = trimmed.find('=');
    if (equals_pos != std::string::npos) {
        std::string key = trim(trimmed.substr(0, equals_pos));
        std::string value = trim(trimmed.substr(equals_pos + 1));

        // Remove quotes from string values
        if (value.length() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.length() - 2);
        }

        sections_[current_section][key] = value;
    }
}

bool TomlParser::parse() {
    std::istringstream stream(content_);
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        parseLine(line, current_section);
    }

    return true;
}

std::optional<int> TomlParser::getInt(const std::string& section, const std::string& key) const {
    auto section_it = sections_.find(section);
    if (section_it == sections_.end()) {
        return std::nullopt;
    }

    auto key_it = section_it->second.find(key);
    if (key_it == section_it->second.end()) {
        return std::nullopt;
    }

    try {
        return std::stoi(key_it->second);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> TomlParser::getString(const std::string& section, const std::string& key) const {
    auto section_it = sections_.find(section);
    if (section_it == sections_.end()) {
        return std::nullopt;
    }

    auto key_it = section_it->second.find(key);
    if (key_it == section_it->second.end()) {
        return std::nullopt;
    }

    return key_it->second;
}

std::optional<bool> TomlParser::getBool(const std::string& section, const std::string& key) const {
    auto section_it = sections_.find(section);
    if (section_it == sections_.end()) {
        return std::nullopt;
    }

    auto key_it = section_it->second.find(key);
    if (key_it == section_it->second.end()) {
        return std::nullopt;
    }

    std::string value = key_it->second;
    if (value == "true") return true;
    if (value == "false") return false;

    return std::nullopt;
}

// ============================================================================
// FormatterOptions Implementation
// ============================================================================

FormatterOptions FormatterOptions::defaults() {
    FormatterOptions options;
    options.indent_width = 4;
    options.max_line_length = 100;
    options.semicolons = SemicolonStyle::Never;
    options.trailing_commas = true;
    options.function_brace_style = BraceStyle::SameLine;
    options.control_flow_brace_style = BraceStyle::SameLine;
    options.blank_lines_between_declarations = 1;
    options.blank_lines_between_sections = 2;
    options.space_before_function_paren = false;
    options.space_in_empty_parens = false;
    options.wrap_function_params = WrappingStyle::Auto;
    options.wrap_struct_fields = WrappingStyle::Auto;
    options.wrap_array_elements = WrappingStyle::Auto;
    options.align_wrapped_params = true;
    return options;
}

FormatterOptions FormatterOptions::fromToml(const std::string& toml_content) {
    FormatterOptions options = defaults();

    TomlParser parser(toml_content);
    if (!parser.parse()) {
        return options; // Return defaults on parse error
    }

    // Parse [style] section
    if (auto indent = parser.getInt("style", "indent_width")) {
        options.indent_width = *indent;
    }
    if (auto max_len = parser.getInt("style", "max_line_length")) {
        options.max_line_length = *max_len;
    }
    if (auto semi = parser.getString("style", "semicolons")) {
        if (*semi == "always") {
            options.semicolons = SemicolonStyle::Always;
        } else if (*semi == "never") {
            options.semicolons = SemicolonStyle::Never;
        } else if (*semi == "as-needed") {
            options.semicolons = SemicolonStyle::AsNeeded;
        }
    }
    if (auto trailing = parser.getBool("style", "trailing_commas")) {
        options.trailing_commas = *trailing;
    }

    // Parse [braces] section
    if (auto func_brace = parser.getString("braces", "function_brace_style")) {
        if (*func_brace == "same_line") {
            options.function_brace_style = BraceStyle::SameLine;
        } else if (*func_brace == "next_line") {
            options.function_brace_style = BraceStyle::NextLine;
        }
    }
    if (auto ctrl_brace = parser.getString("braces", "control_flow_brace_style")) {
        if (*ctrl_brace == "same_line") {
            options.control_flow_brace_style = BraceStyle::SameLine;
        } else if (*ctrl_brace == "next_line") {
            options.control_flow_brace_style = BraceStyle::NextLine;
        }
    }

    // Parse [spacing] section
    if (auto blank_decl = parser.getInt("spacing", "blank_lines_between_declarations")) {
        options.blank_lines_between_declarations = *blank_decl;
    }
    if (auto blank_sect = parser.getInt("spacing", "blank_lines_between_sections")) {
        options.blank_lines_between_sections = *blank_sect;
    }
    if (auto space_paren = parser.getBool("spacing", "space_before_function_paren")) {
        options.space_before_function_paren = *space_paren;
    }
    if (auto space_empty = parser.getBool("spacing", "space_in_empty_parens")) {
        options.space_in_empty_parens = *space_empty;
    }

    // Parse [wrapping] section
    if (auto wrap_params = parser.getString("wrapping", "wrap_function_params")) {
        if (*wrap_params == "auto") {
            options.wrap_function_params = WrappingStyle::Auto;
        } else if (*wrap_params == "always") {
            options.wrap_function_params = WrappingStyle::Always;
        } else if (*wrap_params == "never") {
            options.wrap_function_params = WrappingStyle::Never;
        }
    }
    if (auto wrap_fields = parser.getString("wrapping", "wrap_struct_fields")) {
        if (*wrap_fields == "auto") {
            options.wrap_struct_fields = WrappingStyle::Auto;
        } else if (*wrap_fields == "always") {
            options.wrap_struct_fields = WrappingStyle::Always;
        } else if (*wrap_fields == "never") {
            options.wrap_struct_fields = WrappingStyle::Never;
        }
    }
    if (auto wrap_array = parser.getString("wrapping", "wrap_array_elements")) {
        if (*wrap_array == "auto") {
            options.wrap_array_elements = WrappingStyle::Auto;
        } else if (*wrap_array == "always") {
            options.wrap_array_elements = WrappingStyle::Always;
        } else if (*wrap_array == "never") {
            options.wrap_array_elements = WrappingStyle::Never;
        }
    }
    if (auto align = parser.getBool("wrapping", "align_wrapped_params")) {
        options.align_wrapped_params = *align;
    }

    return options;
}

FormatterOptions FormatterOptions::fromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return defaults(); // Return defaults if file can't be opened
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    return fromToml(buffer.str());
}

} // namespace formatter
} // namespace naab
