#include "naab/block_registry.h"
#include "naab/audit_logger.h"  // V-RT-004: logHashMismatch on tampered blocks
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <filesystem>

// V-RT-004: block source integrity verification via SHA-256
#ifdef HAVE_OPENSSL
#  include <openssl/sha.h>
static std::string computeBlockSHA256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}
#endif

using json = nlohmann::json;

namespace naab {
namespace runtime {

BlockRegistry& BlockRegistry::instance() {
    static BlockRegistry instance;
    return instance;
}

void BlockRegistry::initialize(const std::string& blocks_path) {
    if (initialized_) {
        return;
    }

    blocks_path_ = blocks_path;
    blocks_.clear();

    // Try loading from cache first (much faster than scanning 24K+ files)
    if (!loadCache(blocks_path_)) {
        // Cache miss or stale — do full directory scan
        scanDirectory(blocks_path_);
        // Save cache for next time
        saveCache(blocks_path_);
    }

    initialized_ = true;
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

            // If no inline code, check for code_file reference
            if (source.empty() && block_json.contains("code_file") && block_json["code_file"].is_string()) {
                std::string code_file = block_json["code_file"].get<std::string>();
                // V-RT-005: Validate code_file is a simple filename (no path traversal)
                if (!code_file.empty() &&
                    code_file[0] != '/' &&
                    code_file.find("..") == std::string::npos &&
                    code_file.find('/') == std::string::npos &&
                    code_file.find('\\') == std::string::npos) {
                    // Resolve relative to the JSON file's directory
                    std::string dir = file_path.substr(0, file_path.find_last_of('/'));
                    std::string code_path = dir + "/" + code_file;
                    // Double-check: canonical path must stay within block directory
                    std::error_code ec;
                    auto canonical_code = std::filesystem::canonical(code_path, ec);
                    auto canonical_dir = std::filesystem::canonical(dir, ec);
                    if (!ec && canonical_code.string().rfind(canonical_dir.string(), 0) == 0) {
                        source = readFile(code_path);
                    }
                }
            }
        } catch (const std::exception& e) {
            return "";
        }
    } else {
        // For regular source files, return the whole file
        source = readFile(file_path);
    }

    // V-RT-004: verify source integrity against the stored code_hash (if available).
    // This detects tampering of block files between registration and execution.
#ifdef HAVE_OPENSSL
    if (!metadata_opt->code_hash.empty()) {
        std::string actual_hash = computeBlockSHA256(source);
        if (actual_hash != metadata_opt->code_hash) {
            naab::security::AuditLogger::logHashMismatch(
                block_id, metadata_opt->code_hash, actual_hash);
            throw std::runtime_error(
                "Block integrity check failed: block '" + block_id + "' has been tampered.\n\n"
                "  Expected hash: " + metadata_opt->code_hash + "\n"
                "  Actual hash:   " + actual_hash + "\n\n"
                "  The block source file does not match the registered code_hash.\n"
                "  Re-install the block or update the hash in the metadata JSON.\n"
            );
        }
    }
#endif

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
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(base_path, ec)) {
        if (ec) break;
        auto name = entry.path().filename().string();
        if (name[0] == '.') continue;

        if (entry.is_directory()) {
            std::string language = name;
            if (language == "c++") language = "cpp";
            scanLanguageDirectory(entry.path().string(), language);
        }
    }
    if (ec) {
        fmt::print("[ERROR] Failed to open blocks directory: {}\n", base_path);
    }
}

void BlockRegistry::scanLanguageDirectory(const std::string& lang_dir, const std::string& language) {
    namespace fs = std::filesystem;
    std::error_code ec;
    int blocks_found = 0;

    for (const auto& dir_entry : fs::directory_iterator(lang_dir, ec)) {
        if (ec) break;
        auto filename = dir_entry.path().filename().string();
        if (filename[0] == '.') continue;
        std::string full_path = dir_entry.path().string();

        if (dir_entry.is_regular_file()) {
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
                    metadata.category = block_json.contains("category") && block_json["category"].is_string() && !block_json["category"].is_null()
                        ? block_json["category"].get<std::string>() : "";
                    metadata.subcategory = block_json.contains("subcategory") && block_json["subcategory"].is_string() && !block_json["subcategory"].is_null()
                        ? block_json["subcategory"].get<std::string>() : "";
                    metadata.code_hash = block_json.contains("code_hash") && block_json["code_hash"].is_string() && !block_json["code_hash"].is_null()
                        ? block_json["code_hash"].get<std::string>() : "";

                    // AI-powered discovery fields (Phase 1.4)
                    metadata.description = block_json.value("description", "");
                    metadata.short_desc = block_json.value("short_desc", "");
                    metadata.input_types = block_json.value("input_types", "");
                    metadata.output_type = block_json.value("output_type", "");

                    // Vector fields with JSON array parsing
                    if (block_json.contains("keywords") && block_json["keywords"].is_array()) {
                        for (const auto& keyword : block_json["keywords"]) {
                            if (keyword.is_string()) metadata.keywords.push_back(keyword.get<std::string>());
                        }
                    }
                    if (block_json.contains("use_cases") && block_json["use_cases"].is_array()) {
                        for (const auto& use_case : block_json["use_cases"]) {
                            if (use_case.is_string()) metadata.use_cases.push_back(use_case.get<std::string>());
                        }
                    }
                    if (block_json.contains("related_blocks") && block_json["related_blocks"].is_array()) {
                        for (const auto& related : block_json["related_blocks"]) {
                            if (related.is_string()) metadata.related_blocks.push_back(related.get<std::string>());
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

                // Don't override JSON-registered blocks with raw source files
                if (blocks_.count(block_id) > 0) {
                    continue;  // JSON metadata takes priority
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

    // Found blocks (silent)
    if (ec) {
        fmt::print("[ERROR] Failed to open language directory: {}\n", lang_dir);
    }
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

bool BlockRegistry::loadCache(const std::string& base_path) {
    namespace fs = std::filesystem;
    std::string cache_path = base_path + "/.block_cache.json";

    std::error_code ec;
    auto cache_time = fs::last_write_time(cache_path, ec);
    if (ec) return false;  // No cache file

    // Check if any language directory is newer than cache
    for (const auto& entry : fs::directory_iterator(base_path, ec)) {
        if (ec) return false;
        auto name = entry.path().filename().string();
        if (name[0] == '.') continue;
        if (entry.is_directory()) {
            auto dir_time = fs::last_write_time(entry.path(), ec);
            if (!ec && dir_time > cache_time) {
                return false;  // Directory modified after cache
            }
        }
    }

    // Load cache
    try {
        std::string content = readFile(cache_path);
        if (content.empty()) return false;

        json cache = json::parse(content);
        if (!cache.contains("version") || cache["version"].get<int>() != 1) {
            return false;  // Wrong version
        }

        auto& blocks = cache["blocks"];
        for (auto it = blocks.begin(); it != blocks.end(); ++it) {
            BlockMetadata metadata;
            const auto& b = it.value();
            metadata.block_id = it.key();
            metadata.name = b.value("name", metadata.block_id);
            metadata.language = b.value("language", "");
            metadata.file_path = b.value("file_path", "");
            metadata.version = b.value("version", "1.0.0");
            metadata.token_count = b.value("token_count", 0);
            metadata.times_used = b.value("times_used", 0);
            metadata.is_active = b.value("is_active", true);
            metadata.category = b.value("category", "");
            metadata.subcategory = b.value("subcategory", "");
            metadata.description = b.value("description", "");
            metadata.short_desc = b.value("short_desc", "");
            metadata.input_types = b.value("input_types", "");
            metadata.output_type = b.value("output_type", "");
            metadata.performance_tier = b.value("performance_tier", "unknown");
            metadata.success_rate_percent = b.value("success_rate_percent", 100);

            blocks_[metadata.block_id] = metadata;
        }

        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void BlockRegistry::saveCache(const std::string& base_path) const {
    std::string cache_path = base_path + "/.block_cache.json";

    try {
        json cache;
        cache["version"] = 1;

        json blocks_json;
        for (const auto& [id, meta] : blocks_) {
            json b;
            b["name"] = meta.name;
            b["language"] = meta.language;
            b["file_path"] = meta.file_path;
            b["version"] = meta.version;
            b["token_count"] = meta.token_count;
            b["is_active"] = meta.is_active;
            b["category"] = meta.category;
            b["description"] = meta.description;
            b["short_desc"] = meta.short_desc;
            b["input_types"] = meta.input_types;
            b["output_type"] = meta.output_type;
            b["performance_tier"] = meta.performance_tier;
            b["success_rate_percent"] = meta.success_rate_percent;
            blocks_json[id] = b;
        }
        cache["blocks"] = blocks_json;

        std::ofstream out(cache_path);
        if (out.is_open()) {
            out << cache.dump();
            out.close();
        }
    } catch (const std::exception&) {
        // Silent failure — cache is optional optimization
    }
}

} // namespace runtime
} // namespace naab
