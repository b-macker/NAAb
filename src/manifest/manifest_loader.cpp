// NAAb Manifest Loader Implementation
#include "naab/logger.h"
// Loads and parses naab.toml configuration files

#include "naab/manifest.h"
#include <toml++/toml.h>
#include <filesystem>
#include <fstream>
#include <fmt/core.h>

namespace naab {
namespace manifest {

std::string ManifestLoader::last_error_ = "";

std::optional<Manifest> ManifestLoader::load(const std::string& file_path) {
    try {
        // Parse TOML file
        auto config = toml::parse_file(file_path);

        Manifest manifest;

        // Parse [package] section
        if (auto package = config["package"]) {
            manifest.package.name = package["name"].value_or("");
            manifest.package.version = package["version"].value_or("0.1.0");

            if (auto authors = package["authors"].as_array()) {
                for (auto& author : *authors) {
                    manifest.package.authors.push_back(author.value_or(""));
                }
            }

            manifest.package.description = package["description"].value_or("");
        }

        // Parse [dependencies] section
        if (auto deps = config["dependencies"].as_table()) {
            for (auto& [key, value] : *deps) {
                manifest.dependencies[std::string(key)] = value.value_or("");
            }
        }

        // Parse [build] section
        if (auto build = config["build"]) {
            manifest.build.target = build["target"].value_or("debug");
            manifest.build.optimize = build["optimize"].value_or(false);
        } else {
            // Default build config
            manifest.build.target = "debug";
            manifest.build.optimize = false;
        }

        // Parse [polyglot] section
        if (auto polyglot = config["polyglot"]) {
            manifest.polyglot.python_enabled = polyglot["python"].value_or(true);
            manifest.polyglot.javascript_enabled = polyglot["javascript"].value_or(true);
            manifest.polyglot.rust_enabled = polyglot["rust"].value_or(true);
            manifest.polyglot.cpp_enabled = polyglot["cpp"].value_or(true);
            manifest.polyglot.csharp_enabled = polyglot["csharp"].value_or(true);
            manifest.polyglot.shell_enabled = polyglot["shell"].value_or(true);
            manifest.polyglot.ruby_enabled = polyglot["ruby"].value_or(true);
            manifest.polyglot.go_enabled = polyglot["go"].value_or(true);
        } else {
            // Default: all languages enabled
            manifest.polyglot.python_enabled = true;
            manifest.polyglot.javascript_enabled = true;
            manifest.polyglot.rust_enabled = true;
            manifest.polyglot.cpp_enabled = true;
            manifest.polyglot.csharp_enabled = true;
            manifest.polyglot.shell_enabled = true;
            manifest.polyglot.ruby_enabled = true;
            manifest.polyglot.go_enabled = true;
        }

        // Parse language-specific configs
        if (auto python = config["python"]) {
            LanguageConfig py_config;
            py_config.version = python["version"].value_or("3.7+");
            if (auto packages = python["packages"].as_array()) {
                for (auto& pkg : *packages) {
                    py_config.packages.push_back(pkg.value_or(""));
                }
            }
            manifest.languages["python"] = py_config;
        }

        if (auto javascript = config["javascript"]) {
            LanguageConfig js_config;
            js_config.version = javascript["runtime"].value_or("quickjs");
            if (auto packages = javascript["packages"].as_array()) {
                for (auto& pkg : *packages) {
                    js_config.packages.push_back(pkg.value_or(""));
                }
            }
            manifest.languages["javascript"] = js_config;
        }

        // Parse [features] section
        if (auto features = config["features"]) {
            manifest.features.async_blocks = features["async_blocks"].value_or(false);
            manifest.features.sandbox_mode = features["sandbox_mode"].value_or(true);
        } else {
            // Default features
            manifest.features.async_blocks = false;
            manifest.features.sandbox_mode = true;
        }

        // Validate manifest
        if (!manifest.validate()) {
            last_error_ = manifest.getError();
            return std::nullopt;
        }

        LOG_DEBUG("[Manifest] Loaded: {} v{}\n",
                   manifest.package.name, manifest.package.version);

        return manifest;

    } catch (const toml::parse_error& err) {
        last_error_ = fmt::format("TOML parse error: {}", err.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        last_error_ = fmt::format("Error loading manifest: {}", e.what());
        return std::nullopt;
    }
}

std::optional<Manifest> ManifestLoader::findAndLoad(const std::string& start_dir) {
    namespace fs = std::filesystem;

    // Search up directory tree for naab.toml
    fs::path dir = fs::absolute(start_dir);

    while (true) {
        fs::path manifest_path = dir / "naab.toml";

        if (fs::exists(manifest_path)) {
            LOG_DEBUG("[Manifest] Found: {}\n", manifest_path.string());
            return load(manifest_path.string());
        }

        // Move up one directory
        fs::path parent = dir.parent_path();
        if (parent == dir) {
            // Reached filesystem root
            last_error_ = "No naab.toml found in directory tree";
            return std::nullopt;
        }
        dir = parent;
    }
}

std::string ManifestLoader::getLastError() {
    return last_error_;
}

bool Manifest::validate() const {
    // Validate package name (required)
    if (package.name.empty()) {
        return false;
    }

    // Validate version format (basic check)
    if (package.version.empty()) {
        return false;
    }

    // Validate build target
    if (build.target != "debug" && build.target != "release") {
        return false;
    }

    return true;
}

std::string Manifest::getError() const {
    if (package.name.empty()) {
        return "Package name is required in [package] section";
    }
    if (package.version.empty()) {
        return "Package version is required in [package] section";
    }
    if (build.target != "debug" && build.target != "release") {
        return "Build target must be 'debug' or 'release'";
    }
    return "Unknown validation error";
}

bool createDefaultManifest(const std::string& file_path) {
    std::ofstream file(file_path);
    if (!file.is_open()) {
        fmt::print("[ERROR] Cannot create file: {}\n", file_path);
        return false;
    }

    file << R"([package]
name = "my-naab-project"
version = "0.1.0"
authors = ["Your Name <you@example.com>"]
description = "My NAAb project"

[dependencies]
# Future: External NAAb packages
# http = "1.0"
# json = "2.1"

[build]
# Build configuration
target = "debug"  # or "release"
optimize = false

[polyglot]
# Which language runtimes to enable
python = true
javascript = true
rust = true
cpp = true
csharp = true
shell = true
ruby = true
go = true

[python]
# Python-specific config
version = "3.7+"
packages = []

[javascript]
# JavaScript-specific config
runtime = "quickjs"  # or "node"
packages = []

[features]
# Feature flags
async_blocks = false
sandbox_mode = true
)";

    file.close();
    LOG_DEBUG("[Manifest] Created default naab.toml at: {}\n", file_path);
    return true;
}

} // namespace manifest
} // namespace naab
