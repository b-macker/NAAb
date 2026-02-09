#include "naab/block_registry.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

// Use POSIX directory operations for Android compatibility
#include <dirent.h>
#include <sys/stat.h>

using json = nlohmann::json;

namespace naab {
namespace runtime {

BlockRegistry& BlockRegistry::instance() {
    static BlockRegistry instance;
    return instance;
}

void BlockRegistry::initialize(const std::string& blocks_path) {
    if (initialized_) {
        // BlockRegistry already initialized (silent)
        return;
    }

    blocks_path_ = blocks_path;
    blocks_.clear();

    // Initializing BlockRegistry (silent)

    // Scan the blocks directory
    scanDirectory(blocks_path_);

    initialized_ = true;
    // BlockRegistry initialized (silent)
}

std::optional<BlockMetadata> BlockRegistry::getBlock(const std::string& block_id) const {
    auto it = blocks_.find(block_id);
    if (it != blocks_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::string BlockRegistry::getBlockSource(const std::string& block_id) const {
    // Check cache first to avoid repeated filesystem reads
    auto cache_it = source_cache_.find(block_id);
    if (cache_it != source_cache_.end()) {
        return cache_it->second;
    }

    auto metadata_opt = getBlock(block_id);
    if (!metadata_opt) {
        return "";
    }

    const std::string& file_path = metadata_opt->file_path;
    std::string source;

    // Check if it's a JSON file
    if (file_path.size() > 5 && file_path.substr(file_path.size() - 5) == ".json") {
        try {
            std::string json_content = readFile(file_path);
            if (json_content.empty()) {
                return "";
            }

            json block_json = json::parse(json_content);
            source = block_json.value("code", "");
        } catch (const std::exception& e) {
            return "";
        }
    } else {
        // For regular source files, return the whole file
        source = readFile(file_path);
    }

    // Cache the source for future lookups
    source_cache_[block_id] = source;
    return source;
}

std::vector<std::string> BlockRegistry::listBlocks() const {
    std::vector<std::string> result;
    result.reserve(blocks_.size());

    for (const auto& [id, metadata] : blocks_) {
        result.push_back(id);
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> BlockRegistry::listBlocksByLanguage(const std::string& language) const {
    std::vector<std::string> result;

    for (const auto& [id, metadata] : blocks_) {
        if (metadata.language == language) {
            result.push_back(id);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> BlockRegistry::supportedLanguages() const {
    std::vector<std::string> langs;

    // Collect unique languages
    for (const auto& [id, metadata] : blocks_) {
        if (std::find(langs.begin(), langs.end(), metadata.language) == langs.end()) {
            langs.push_back(metadata.language);
        }
    }

    std::sort(langs.begin(), langs.end());
    return langs;
}

void BlockRegistry::scanDirectory(const std::string& base_path) {
    DIR* dir = opendir(base_path.c_str());
    if (!dir) {
        fmt::print("[ERROR] Failed to open blocks directory: {}\n", base_path);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string entry_name = entry->d_name;
        std::string full_path = base_path + "/" + entry_name;

        // Check if it's a directory
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            // This is a language directory (e.g., "cpp", "javascript", "c++")
            std::string language = entry_name;

            // Normalize c++ to cpp
            if (language == "c++") {
                language = "cpp";
            }

            // Scanning language directory (silent)
            scanLanguageDirectory(full_path, language);
        }
    }

    closedir(dir);
}

void BlockRegistry::scanLanguageDirectory(const std::string& lang_dir, const std::string& language) {
    DIR* dir = opendir(lang_dir.c_str());
    if (!dir) {
        fmt::print("[ERROR] Failed to open language directory: {}\n", lang_dir);
        return;
    }

    int blocks_found = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string filename = entry->d_name;
        std::string full_path = lang_dir + "/" + filename;

        // Check if it's a regular file
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            // Check if it's a JSON block file
            if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".json") {
                // Parse JSON block metadata
                try {
                    std::string json_content = readFile(full_path);
                    if (json_content.empty()) {
                        continue;
                    }

                    json block_json = json::parse(json_content);

                    // Create metadata from JSON
                    BlockMetadata metadata;
                    metadata.block_id = block_json.value("id", "");
                    metadata.name = block_json.value("name", metadata.block_id);
                    metadata.language = language;  // Use normalized language
                    metadata.file_path = full_path;
                    metadata.version = block_json.value("version", "1.0.0");
                    metadata.token_count = block_json.value("token_count", 0);
                    metadata.times_used = block_json.value("times_used", 0);
                    metadata.is_active = block_json.value("is_active", true);

                    // Handle potentially null fields
                    metadata.category = block_json.contains("category") && !block_json["category"].is_null()
                        ? block_json["category"].get<std::string>() : "";
                    metadata.subcategory = block_json.contains("subcategory") && !block_json["subcategory"].is_null()
                        ? block_json["subcategory"].get<std::string>() : "";
                    metadata.code_hash = block_json.contains("code_hash") && !block_json["code_hash"].is_null()
                        ? block_json["code_hash"].get<std::string>() : "";

                    // AI-powered discovery fields (Phase 1.4)
                    metadata.description = block_json.value("description", "");
                    metadata.short_desc = block_json.value("short_desc", "");
                    metadata.input_types = block_json.value("input_types", "");
                    metadata.output_type = block_json.value("output_type", "");

                    // Vector fields with JSON array parsing
                    if (block_json.contains("keywords") && block_json["keywords"].is_array()) {
                        for (const auto& keyword : block_json["keywords"]) {
                            metadata.keywords.push_back(keyword.get<std::string>());
                        }
                    }
                    if (block_json.contains("use_cases") && block_json["use_cases"].is_array()) {
                        for (const auto& use_case : block_json["use_cases"]) {
                            metadata.use_cases.push_back(use_case.get<std::string>());
                        }
                    }
                    if (block_json.contains("related_blocks") && block_json["related_blocks"].is_array()) {
                        for (const auto& related : block_json["related_blocks"]) {
                            metadata.related_blocks.push_back(related.get<std::string>());
                        }
                    }

                    // Performance and quality metrics
                    metadata.avg_execution_ms = block_json.value("avg_execution_ms", 0.0);
                    metadata.max_memory_mb = block_json.value("max_memory_mb", 0);
                    metadata.performance_tier = block_json.value("performance_tier", "unknown");
                    metadata.success_rate_percent = block_json.value("success_rate_percent", 100);
                    metadata.avg_tokens_saved = block_json.value("avg_tokens_saved", 0);

                    // Quality assurance fields
                    metadata.test_coverage_percent = block_json.value("test_coverage_percent", 0);
                    metadata.security_audited = block_json.value("security_audited", false);
                    metadata.stability = block_json.value("stability", "stable");

                    // Store in registry
                    if (!metadata.block_id.empty()) {
                        blocks_[metadata.block_id] = metadata;
                        blocks_found++;
                    }
                } catch (const std::exception& e) {
                    // Skip malformed JSON files
                    fmt::print("[DEBUG] Failed to parse JSON file {}: {}\n", filename, e.what());
                    continue;
                }

            } else {
                // Handle source code files (.cpp, .js, .py, etc.)
                std::string detected_lang = detectLanguageFromExtension(filename);
                if (detected_lang.empty() || detected_lang != language) {
                    continue;
                }

                std::string block_id = extractBlockId(filename);
                if (block_id.empty()) {
                    continue;
                }

                BlockMetadata metadata;
                metadata.block_id = block_id;
                metadata.name = block_id;
                metadata.language = language;
                metadata.file_path = full_path;
                metadata.version = "1.0.0";
                metadata.token_count = 0;
                metadata.times_used = 0;
                metadata.is_active = true;
                metadata.category = "";
                metadata.subcategory = "";
                metadata.code_hash = "";

                // AI-powered discovery fields - initialize with defaults
                metadata.description = "";
                metadata.short_desc = "";
                metadata.input_types = "";
                metadata.output_type = "";
                // keywords, use_cases, related_blocks vectors are empty by default

                // Performance and quality metrics - defaults
                metadata.avg_execution_ms = 0.0;
                metadata.max_memory_mb = 0;
                metadata.performance_tier = "unknown";
                metadata.success_rate_percent = 100;
                metadata.avg_tokens_saved = 0;

                // Quality assurance - defaults
                metadata.test_coverage_percent = 0;
                metadata.security_audited = false;
                metadata.stability = "stable";

                blocks_[block_id] = metadata;
                blocks_found++;
            }
        }
    }

    closedir(dir);
    // Found blocks (silent)
}

std::string BlockRegistry::extractBlockId(const std::string& filename) const {
    // Find last dot
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return filename;  // No extension
    }

    // Return filename without extension
    return filename.substr(0, dot_pos);
}

std::string BlockRegistry::detectLanguageFromExtension(const std::string& filename) const {
    // Find extension
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "";
    }

    std::string ext = filename.substr(dot_pos + 1);

    // Map extensions to languages
    if (ext == "cpp" || ext == "cc" || ext == "cxx") {
        return "cpp";
    } else if (ext == "js") {
        return "javascript";
    } else if (ext == "py") {
        return "python";
    } else if (ext == "rs") {
        return "rust";
    } else if (ext == "go") {
        return "go";
    }

    return "";
}

std::string BlockRegistry::readFile(const std::string& file_path) const {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        fmt::print("[ERROR] Failed to open file: {}\n", file_path);
        return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace runtime
} // namespace naab
