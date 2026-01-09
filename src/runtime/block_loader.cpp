// NAAb Block Loader Implementation
// SQLite3-based block registry access

#include "naab/block_loader.h"
#include "naab/config.h"
#include "naab/semver.h"
#include "naab/sandbox.h"
#include <sqlite3.h>
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <map>

namespace naab {
namespace runtime {

// PIMPL implementation to hide SQLite3 details
class BlockLoader::Impl {
public:
    sqlite3* db_;
    std::string blocks_dir_;

    explicit Impl(const std::string& db_path) : db_(nullptr) {
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::string error = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            throw std::runtime_error("Failed to open database: " + error);
        }

        // Extract blocks directory from database path
        // /path/to/naab/data/naab.db → /path/to/naab/blocks/
        size_t last_slash = db_path.rfind('/');
        if (last_slash != std::string::npos) {
            blocks_dir_ = db_path.substr(0, last_slash);
            // Remove "/data" and add "/blocks"
            size_t data_pos = blocks_dir_.rfind("/data");
            if (data_pos != std::string::npos) {
                blocks_dir_ = blocks_dir_.substr(0, data_pos) + "/blocks";
            }
        }
    }

    ~Impl() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    BlockMetadata parseRow(sqlite3_stmt* stmt) {
        BlockMetadata meta;
        meta.block_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        meta.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        meta.language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        const char* category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        meta.category = category ? category : "";

        const char* subcategory = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        meta.subcategory = subcategory ? subcategory : "";

        meta.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        meta.code_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        meta.token_count = sqlite3_column_int(stmt, 7);
        meta.times_used = sqlite3_column_int(stmt, 8);

        const char* version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        meta.version = version ? version : "1.0";

        meta.is_active = sqlite3_column_int(stmt, 15) != 0;

        // Versioning fields (with safe defaults if columns don't exist)
        meta.min_runtime_version = "";  // No requirement by default
        meta.deprecated = false;
        meta.deprecated_reason = "";
        meta.replacement_block_id = "";

        // AI-powered discovery fields (Phase 1.4) - now from database
        const char* desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
        meta.description = desc ? desc : "";

        const char* short_d = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));
        meta.short_desc = short_d ? short_d : "";

        const char* in_types = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 18));
        meta.input_types = in_types ? in_types : "";

        const char* out_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 19));
        meta.output_type = out_type ? out_type : "";

        // TODO: Parse JSON arrays for keywords, use_cases, related_blocks (columns 20-22)
        // For now, leave as empty vectors

        // Performance and quality metrics - from database
        meta.avg_execution_ms = sqlite3_column_double(stmt, 23);
        meta.max_memory_mb = sqlite3_column_int(stmt, 24);

        const char* perf_tier = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 25));
        meta.performance_tier = perf_tier ? perf_tier : "medium";

        meta.success_rate_percent = sqlite3_column_int(stmt, 26);
        meta.avg_tokens_saved = sqlite3_column_int(stmt, 27);

        // Quality assurance - from database
        meta.test_coverage_percent = sqlite3_column_int(stmt, 28);
        meta.security_audited = sqlite3_column_int(stmt, 29) != 0;
        meta.stability = "stable";  // Not in DB yet, default

        return meta;
    }
};

BlockLoader::BlockLoader(const std::string& db_path)
    : pimpl_(std::make_unique<Impl>(db_path)) {
}

BlockLoader::~BlockLoader() = default;

BlockMetadata BlockLoader::getBlock(const std::string& block_id) {
    const char* sql = "SELECT block_id, name, language, category, subcategory, "
                     "file_path, code_hash, token_count, times_used, total_tokens_saved, "
                     "version, created_at, last_used, validation_status, tags, is_active, "
                     "description, short_desc, input_types, output_type, keywords, "
                     "use_cases, related_blocks, avg_execution_ms, max_memory_mb, "
                     "performance_tier, success_rate_percent, avg_tokens_saved, "
                     "test_coverage_percent, security_audited "
                     "FROM blocks_registry WHERE block_id = ? LIMIT 1";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare statement: " +
                               std::string(sqlite3_errmsg(pimpl_->db_)));
    }

    sqlite3_bind_text(stmt, 1, block_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Block not found: " + block_id);
    }

    BlockMetadata meta = pimpl_->parseRow(stmt);
    sqlite3_finalize(stmt);

    return meta;
}

std::vector<BlockMetadata> BlockLoader::searchBlocks(const std::string& query) {
    std::vector<BlockMetadata> results;

    const char* sql = "SELECT block_id, name, language, category, subcategory, "
                     "file_path, code_hash, token_count, times_used, total_tokens_saved, "
                     "version, created_at, last_used, validation_status, tags, is_active, "
                     "description, short_desc, input_types, output_type, keywords, "
                     "use_cases, related_blocks, avg_execution_ms, max_memory_mb, "
                     "performance_tier, success_rate_percent, avg_tokens_saved, "
                     "test_coverage_percent, security_audited "
                     "FROM blocks_registry WHERE name LIKE ? OR block_id LIKE ? LIMIT 100";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare statement");
    }

    std::string pattern = "%" + query + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        results.push_back(pimpl_->parseRow(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<BlockMetadata> BlockLoader::getBlocksByLanguage(const std::string& language) {
    std::vector<BlockMetadata> results;

    const char* sql = "SELECT block_id, name, language, category, subcategory, "
                     "file_path, code_hash, token_count, times_used, total_tokens_saved, "
                     "version, created_at, last_used, validation_status, tags, is_active, "
                     "description, short_desc, input_types, output_type, keywords, "
                     "use_cases, related_blocks, avg_execution_ms, max_memory_mb, "
                     "performance_tier, success_rate_percent, avg_tokens_saved, "
                     "test_coverage_percent, security_audited "
                     "FROM blocks_registry WHERE language = ? LIMIT 1000";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare statement");
    }

    sqlite3_bind_text(stmt, 1, language.c_str(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        results.push_back(pimpl_->parseRow(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

int BlockLoader::getTotalBlocks() const {
    const char* sql = "SELECT COUNT(*) FROM blocks_registry WHERE is_active = 1";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

std::string BlockLoader::loadBlockCode(const std::string& block_id) {
    // Check sandbox permissions for block loading
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->canLoadBlock(block_id)) {
        fmt::print("[ERROR] Sandbox violation: Block loading denied: {}\n", block_id);
        sandbox->logViolation("loadBlock", block_id, "BLOCK_LOAD capability required");
        throw std::runtime_error("Block loading denied by sandbox: " + block_id);
    }

    // Get block metadata to find file path
    BlockMetadata meta = getBlock(block_id);

    // file_path contains the full absolute path to the block file
    std::string full_path = meta.file_path;

    // Check sandbox permissions for file reading
    if (sandbox && !sandbox->canRead(full_path)) {
        fmt::print("[ERROR] Sandbox violation: File read denied: {}\n", full_path);
        sandbox->logViolation("readFile", full_path, "FS_READ capability required");
        throw std::runtime_error("File read denied by sandbox: " + full_path);
    }

    // Read file
    std::ifstream file(full_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open block file: " + full_path);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_content = buffer.str();

    // Extract "code" field from JSON (simple string search)
    // JSON format: "code": "actual code here"
    size_t code_pos = json_content.find("\"code\": \"");
    if (code_pos == std::string::npos) {
        throw std::runtime_error("No 'code' field found in block JSON");
    }

    code_pos += 9;  // Skip '"code": "'
    size_t end_pos = code_pos;
    int brace_depth = 0;

    // Find end of code string (handle escaped quotes)
    while (end_pos < json_content.size()) {
        if (json_content[end_pos] == '\\' && end_pos + 1 < json_content.size()) {
            end_pos += 2;  // Skip escaped character
            continue;
        }
        if (json_content[end_pos] == '"') {
            break;
        }
        end_pos++;
    }

    std::string code = json_content.substr(code_pos, end_pos - code_pos);

    // Unescape the code string (handle \\n, \\t, \\\", etc.)
    std::string unescaped;
    for (size_t i = 0; i < code.size(); i++) {
        if (code[i] == '\\' && i + 1 < code.size()) {
            char next = code[i + 1];
            if (next == 'n') { unescaped += '\n'; i++; }
            else if (next == 't') { unescaped += '\t'; i++; }
            else if (next == 'r') { unescaped += '\r'; i++; }
            else if (next == '"') { unescaped += '"'; i++; }
            else if (next == '\\') { unescaped += '\\'; i++; }
            else { unescaped += code[i]; }
        } else {
            unescaped += code[i];
        }
    }

    return unescaped;
}

void BlockLoader::recordBlockUsage(const std::string& block_id, int tokens_saved) {
    const char* sql = "UPDATE blocks_registry SET times_used = times_used + 1, "
                     "total_tokens_saved = total_tokens_saved + ?, "
                     "last_used = CURRENT_TIMESTAMP WHERE block_id = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        // Non-fatal - just log
        fmt::print("[WARN] Failed to record block usage: {}\n", block_id);
        return;
    }

    sqlite3_bind_int(stmt, 1, tokens_saved);
    sqlite3_bind_text(stmt, 2, block_id.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<BlockMetadata> BlockLoader::getTopBlocksByUsage(int limit) {
    std::vector<BlockMetadata> top_blocks;

    const char* sql = "SELECT block_id, name, language, category, subcategory, "
                     "file_path, code_hash, token_count, times_used, total_tokens_saved, "
                     "version, created_at, last_used, validation_status, tags, is_active "
                     "FROM blocks_registry WHERE is_active = 1 "
                     "ORDER BY times_used DESC LIMIT ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        fmt::print("[ERROR] Failed to prepare top blocks query: {}\n",
                   sqlite3_errmsg(pimpl_->db_));
        return top_blocks;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BlockMetadata meta;
        meta.block_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        meta.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        meta.language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        meta.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        meta.subcategory = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        meta.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        meta.code_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        meta.token_count = sqlite3_column_int(stmt, 7);
        meta.times_used = sqlite3_column_int(stmt, 8);

        top_blocks.push_back(meta);
    }

    sqlite3_finalize(stmt);
    return top_blocks;
}

std::map<std::string, int> BlockLoader::getLanguageStats() {
    std::map<std::string, int> stats;

    const char* sql = "SELECT language, COUNT(*) as count FROM blocks_registry "
                     "WHERE is_active = 1 GROUP BY language ORDER BY count DESC";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        fmt::print("[ERROR] Failed to prepare language stats query: {}\n",
                   sqlite3_errmsg(pimpl_->db_));
        return stats;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        int count = sqlite3_column_int(stmt, 1);
        stats[language] = count;
    }

    sqlite3_finalize(stmt);
    return stats;
}

long long BlockLoader::getTotalTokensSaved() {
    const char* sql = "SELECT COALESCE(SUM(total_tokens_saved), 0) FROM blocks_registry "
                     "WHERE is_active = 1";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        fmt::print("[ERROR] Failed to prepare tokens saved query: {}\n",
                   sqlite3_errmsg(pimpl_->db_));
        return 0;
    }

    long long total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return total;
}

// Phase 4.4: Block pair tracking for usage analytics
void BlockLoader::recordBlockPair(const std::string& block1_id, const std::string& block2_id) {
    // Create block_pairs table if it doesn't exist
    const char* create_sql =
        "CREATE TABLE IF NOT EXISTS block_pairs ("
        "    block1_id TEXT NOT NULL,"
        "    block2_id TEXT NOT NULL,"
        "    times_used INTEGER DEFAULT 0,"
        "    last_used TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "    PRIMARY KEY (block1_id, block2_id)"
        ")";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(pimpl_->db_, create_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        fmt::print("[WARN] Failed to create block_pairs table: {}\n", err_msg);
        sqlite3_free(err_msg);
        return;
    }

    // Insert or update the pair
    const char* sql =
        "INSERT INTO block_pairs (block1_id, block2_id, times_used, last_used) "
        "VALUES (?, ?, 1, CURRENT_TIMESTAMP) "
        "ON CONFLICT(block1_id, block2_id) DO UPDATE SET "
        "times_used = times_used + 1, last_used = CURRENT_TIMESTAMP";

    sqlite3_stmt* stmt;
    rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        fmt::print("[WARN] Failed to record block pair: {}\n", sqlite3_errmsg(pimpl_->db_));
        return;
    }

    sqlite3_bind_text(stmt, 1, block1_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, block2_id.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<std::pair<std::string, std::string>> BlockLoader::getTopCombinations(int limit) {
    std::vector<std::pair<std::string, std::string>> combinations;

    // First ensure table exists
    const char* create_sql =
        "CREATE TABLE IF NOT EXISTS block_pairs ("
        "    block1_id TEXT NOT NULL,"
        "    block2_id TEXT NOT NULL,"
        "    times_used INTEGER DEFAULT 0,"
        "    last_used TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "    PRIMARY KEY (block1_id, block2_id)"
        ")";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(pimpl_->db_, create_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        fmt::print("[WARN] Failed to create block_pairs table: {}\n", err_msg);
        sqlite3_free(err_msg);
        return combinations;
    }

    // Query top combinations
    const char* sql =
        "SELECT block1_id, block2_id FROM block_pairs "
        "ORDER BY times_used DESC LIMIT ?";

    sqlite3_stmt* stmt;
    rc = sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        fmt::print("[ERROR] Failed to prepare top combinations query: {}\n",
                   sqlite3_errmsg(pimpl_->db_));
        return combinations;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string block1 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string block2 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        combinations.push_back({block1, block2});
    }

    sqlite3_finalize(stmt);
    return combinations;
}

// ============================================================================
// BlockMetadata Version Helper Methods
// ============================================================================

versioning::SemanticVersion BlockMetadata::getSemanticVersion() const {
    if (version.empty() || version == "1.0") {
        // Default version
        return versioning::SemanticVersion(1, 0, 0);
    }

    try {
        return versioning::SemanticVersion::parse(version);
    } catch (const versioning::VersionParseException& e) {
        fmt::print("[WARN] Invalid block version '{}' for {}: {}\n",
                   version, block_id, e.what());
        return versioning::SemanticVersion(1, 0, 0);
    }
}

bool BlockMetadata::isCompatibleWithRuntime() const {
    if (min_runtime_version.empty()) {
        return true;  // No requirement specified
    }

    try {
        versioning::SemanticVersion runtime_ver =
            versioning::SemanticVersion::parse(NAAB_VERSION_STRING);

        return runtime_ver.satisfiesRange(min_runtime_version);
    } catch (const versioning::VersionParseException& e) {
        fmt::print("[WARN] Invalid min_runtime_version '{}' for {}: {}\n",
                   min_runtime_version, block_id, e.what());
        return true;  // Allow if version is invalid
    }
}

// ============================================================================
// BlockLoader Version Checking Methods (Static)
// ============================================================================

bool BlockLoader::checkBlockCompatibility(const BlockMetadata& block) {
    // Check if block specifies minimum runtime version
    if (block.min_runtime_version.empty()) {
        return true;  // No requirement
    }

    try {
        // Parse runtime version
        versioning::SemanticVersion runtime_version =
            versioning::SemanticVersion::parse(NAAB_VERSION_STRING);

        // Check if runtime satisfies block's requirements
        if (!runtime_version.satisfiesRange(block.min_runtime_version)) {
            fmt::print("[ERROR] Block {} (v{}) requires NAAb {}, but running v{}\n",
                       block.block_id, block.version,
                       block.min_runtime_version, NAAB_VERSION_STRING);
            return false;
        }

        return true;
    } catch (const versioning::VersionParseException& e) {
        fmt::print("[WARN] Invalid version in block {}: {}\n",
                   block.block_id, e.what());
        return true;  // Allow on parse failure
    }
}

void BlockLoader::warnDeprecated(const BlockMetadata& block) {
    if (!block.deprecated) {
        return;  // Not deprecated
    }

    // Print formatted deprecation warning to stderr
    std::string warning = formatDeprecationWarning(block);
    fmt::print(stderr, "{}", warning);
}

std::string BlockLoader::formatDeprecationWarning(const BlockMetadata& block) {
    if (!block.deprecated) {
        return "";
    }

    // Create boxed deprecation warning
    std::string warning =
        "╔════════════════════════════════════════════════════════════╗\n"
        "║ DEPRECATION WARNING                                        ║\n"
        "╠════════════════════════════════════════════════════════════╣\n";

    // Block ID and version
    warning += fmt::format("║ Block: {:<50} ║\n", block.block_id);
    warning += fmt::format("║ Version: {:<48} ║\n", block.version);

    // Deprecation reason (word wrap if needed)
    if (!block.deprecated_reason.empty()) {
        std::string reason = block.deprecated_reason;
        if (reason.length() > 49) {
            reason = reason.substr(0, 46) + "...";
        }
        warning += fmt::format("║ Reason: {:<49} ║\n", reason);
    }

    // Replacement suggestion
    if (!block.replacement_block_id.empty()) {
        std::string replacement = block.replacement_block_id;
        if (replacement.length() > 44) {
            replacement = replacement.substr(0, 41) + "...";
        }
        warning += fmt::format("║ Replacement: {:<44} ║\n", replacement);
    }

    warning += "╚════════════════════════════════════════════════════════════╝\n";

    return warning;
}

} // namespace runtime
} // namespace naab
