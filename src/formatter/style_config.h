#pragma once

#include <string>
#include <map>
#include <optional>

namespace naab {
namespace formatter {

// Simple TOML parser for formatter configuration
class TomlParser {
public:
    explicit TomlParser(const std::string& content);

    // Parse TOML content
    bool parse();

    // Get values
    std::optional<int> getInt(const std::string& section, const std::string& key) const;
    std::optional<std::string> getString(const std::string& section, const std::string& key) const;
    std::optional<bool> getBool(const std::string& section, const std::string& key) const;

    // Get error message
    const std::string& getError() const { return error_; }

private:
    std::string content_;
    std::map<std::string, std::map<std::string, std::string>> sections_;
    std::string error_;

    void parseLine(const std::string& line, std::string& current_section);
    std::string trim(const std::string& str) const;
};

} // namespace formatter
} // namespace naab

