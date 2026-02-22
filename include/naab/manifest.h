#pragma once

// NAAb Manifest System
// Loads and manages naab.toml project configuration

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace naab {
namespace manifest {

// Package metadata
struct PackageInfo {
    std::string name;
    std::string version;
    std::vector<std::string> authors;
    std::string description;
};

// Build configuration
struct BuildConfig {
    std::string target;  // "debug" or "release"
    bool optimize;
};

// Polyglot language enablement
struct PolyglotConfig {
    bool python_enabled;
    bool javascript_enabled;
    bool rust_enabled;
    bool cpp_enabled;
    bool csharp_enabled;
    bool shell_enabled;
    bool ruby_enabled;
    bool go_enabled;
};

// Language-specific configuration
struct LanguageConfig {
    std::string version;
    std::vector<std::string> packages;
};

// Feature flags
struct FeatureFlags {
    bool async_blocks;
    bool sandbox_mode;
};

// Main manifest structure
struct Manifest {
    PackageInfo package;
    std::unordered_map<std::string, std::string> dependencies;
    BuildConfig build;
    PolyglotConfig polyglot;
    std::unordered_map<std::string, LanguageConfig> languages;
    FeatureFlags features;

    // Validation
    bool validate() const;
    std::string getError() const;
};

// Manifest loading
class ManifestLoader {
public:
    // Load manifest from specific file path
    static std::optional<Manifest> load(const std::string& file_path);

    // Find and load naab.toml by searching up directory tree
    static std::optional<Manifest> findAndLoad(const std::string& start_dir);

    // Get last error message
    static std::string getLastError();

private:
    static std::string last_error_;
};

// Create default naab.toml file
bool createDefaultManifest(const std::string& file_path);

} // namespace manifest
} // namespace naab

