// NAAb Block Search Index Implementation (Phase 1.5)
// SQLite FTS5-based full-text search for 24,488 blocks
// Target: <100ms search latency

#include "naab/block_search_index.h"
#include <sqlite3.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>

using json = nlohmann::json;

namespace naab {
namespace runtime {

// PIMPL implementation to hide SQLite details
class BlockSearchIndex::Impl {
public:
    sqlite3* db_ = nullptr;

    explicit Impl(const std::string& db_path) {
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::string error = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            throw std::runtime_error("Failed to open search index database: " + error);
        }

        // Enable WAL mode for better concurrency
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

        // Create schema if not exists
        createSchema();
    }

    ~Impl() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    void createSchema() {
        const char* schema = R"(
            -- Main blocks table with all metadata
            CREATE TABLE IF NOT EXISTS blocks (
                block_id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                language TEXT NOT NULL,
                category TEXT,
                subcategory TEXT,
                file_path TEXT NOT NULL,
                code_hash TEXT,
                token_count INTEGER DEFAULT 0,
                times_used INTEGER DEFAULT 0,
                version TEXT DEFAULT '1.0.0',

                -- AI discovery fields
                description TEXT,
                short_desc TEXT,
                input_types TEXT,
                output_type TEXT,

                -- Performance metrics
                avg_execution_ms REAL DEFAULT 0.0,
                max_memory_mb INTEGER DEFAULT 0,
                performance_tier TEXT DEFAULT 'unknown',
                success_rate_percent INTEGER DEFAULT 100,
                avg_tokens_saved INTEGER DEFAULT 0,

                -- Quality metrics
                test_coverage_percent INTEGER DEFAULT 0,
                security_audited INTEGER DEFAULT 0,
                stability TEXT DEFAULT 'stable',

                is_active INTEGER DEFAULT 1
            );

            -- FTS5 virtual table for full-text search
            CREATE VIRTUAL TABLE IF NOT EXISTS blocks_fts USING fts5(
                block_id UNINDEXED,
                name,
                description,
                short_desc,
                keywords,
                use_cases,
                content='blocks',
                content_rowid='rowid'
            );

            -- Separate tables for vector fields (many-to-many)
            CREATE TABLE IF NOT EXISTS block_keywords (
                block_id TEXT NOT NULL,
                keyword TEXT NOT NULL,
                FOREIGN KEY (block_id) REFERENCES blocks(block_id) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS block_use_cases (
                block_id TEXT NOT NULL,
                use_case TEXT NOT NULL,
                FOREIGN KEY (block_id) REFERENCES blocks(block_id) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS block_related (
                block_id TEXT NOT NULL,
                related_block_id TEXT NOT NULL,
                FOREIGN KEY (block_id) REFERENCES blocks(block_id) ON DELETE CASCADE
            );

            -- Indexes for fast filtering and ranking
            CREATE INDEX IF NOT EXISTS idx_blocks_language ON blocks(language);
            CREATE INDEX IF NOT EXISTS idx_blocks_category ON blocks(category);
            CREATE INDEX IF NOT EXISTS idx_blocks_performance ON blocks(performance_tier);
            CREATE INDEX IF NOT EXISTS idx_blocks_success_rate ON blocks(success_rate_percent);
            CREATE INDEX IF NOT EXISTS idx_blocks_times_used ON blocks(times_used);
            CREATE INDEX IF NOT EXISTS idx_keywords_block ON block_keywords(block_id);
            CREATE INDEX IF NOT EXISTS idx_keywords_keyword ON block_keywords(keyword);

            -- Block usage tracking table
            CREATE TABLE IF NOT EXISTS block_usage (
                block_id TEXT NOT NULL,
                timestamp INTEGER DEFAULT (strftime('%s', 'now')),
                tokens_saved INTEGER DEFAULT 0,
                FOREIGN KEY (block_id) REFERENCES blocks(block_id) ON DELETE CASCADE
            );

            -- Block pairs (blocks used together) for recommendations
            CREATE TABLE IF NOT EXISTS block_pairs (
                block_id_1 TEXT NOT NULL,
                block_id_2 TEXT NOT NULL,
                pair_count INTEGER DEFAULT 1,
                PRIMARY KEY (block_id_1, block_id_2)
            );
        )";

        char* error_msg = nullptr;
        int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &error_msg);
        if (rc != SQLITE_OK) {
            std::string error = error_msg ? error_msg : "Unknown error";
            sqlite3_free(error_msg);
            throw std::runtime_error("Failed to create schema: " + error);
        }
    }

    int buildIndex(const std::string& blocks_path) {
        // Building search index (silent)

        // Begin transaction for faster inserts
        sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

        int indexed_count = 0;

        // Scan all language directories
        DIR* dir = opendir(blocks_path.c_str());
        if (!dir) {
            fmt::print("[ERROR] Failed to open blocks directory: {}\n", blocks_path);
            return 0;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;

            std::string lang_name = entry->d_name;
            std::string lang_dir = blocks_path + "/" + lang_name;

            struct stat st;
            if (stat(lang_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                indexed_count += scanLanguageDirectory(lang_dir, lang_name);
            }
        }
        closedir(dir);

        // Commit transaction
        sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

        // Indexed blocks (silent)
        return indexed_count;
    }

    int scanLanguageDirectory(const std::string& lang_dir, const std::string& language) {
        int count = 0;

        DIR* dir = opendir(lang_dir.c_str());
        if (!dir) return 0;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;

            std::string filename = entry->d_name;
            if (filename.size() < 5 || filename.substr(filename.size() - 5) != ".json") {
                continue;
            }

            std::string file_path = lang_dir + "/" + filename;

            // Parse and index the block
            if (indexBlockFile(file_path, language)) {
                count++;
            }
        }
        closedir(dir);

        return count;
    }

    bool indexBlockFile(const std::string& file_path, const std::string& language) {
        try {
            // Read JSON file
            std::ifstream file(file_path);
            if (!file.is_open()) return false;

            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string json_content = buffer.str();

            json block_json = json::parse(json_content);

            // Extract metadata
            std::string block_id = block_json.value("id", "");
            if (block_id.empty()) return false;

            // Insert into blocks table
            const char* sql = R"(
                INSERT OR REPLACE INTO blocks (
                    block_id, name, language, category, subcategory, file_path,
                    code_hash, token_count, times_used, version,
                    description, short_desc, input_types, output_type,
                    avg_execution_ms, max_memory_mb, performance_tier,
                    success_rate_percent, avg_tokens_saved,
                    test_coverage_percent, security_audited, stability, is_active
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            )";

            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                return false;
            }

            // Bind values
            sqlite3_bind_text(stmt, 1, block_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, block_json.value("name", block_id).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, language.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, block_json.value("category", "").c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, block_json.value("subcategory", "").c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, file_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, block_json.value("code_hash", "").c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 8, block_json.value("token_count", 0));
            sqlite3_bind_int(stmt, 9, block_json.value("times_used", 0));
            sqlite3_bind_text(stmt, 10, block_json.value("version", "1.0.0").c_str(), -1, SQLITE_TRANSIENT);

            // AI discovery fields
            sqlite3_bind_text(stmt, 11, block_json.value("description", "").c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 12, block_json.value("short_desc", "").c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 13, block_json.value("input_types", "").c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 14, block_json.value("output_type", "").c_str(), -1, SQLITE_TRANSIENT);

            // Performance metrics
            sqlite3_bind_double(stmt, 15, block_json.value("avg_execution_ms", 0.0));
            sqlite3_bind_int(stmt, 16, block_json.value("max_memory_mb", 0));
            sqlite3_bind_text(stmt, 17, block_json.value("performance_tier", "unknown").c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 18, block_json.value("success_rate_percent", 100));
            sqlite3_bind_int(stmt, 19, block_json.value("avg_tokens_saved", 0));

            // Quality metrics
            sqlite3_bind_int(stmt, 20, block_json.value("test_coverage_percent", 0));
            sqlite3_bind_int(stmt, 21, block_json.value("security_audited", false) ? 1 : 0);
            sqlite3_bind_text(stmt, 22, block_json.value("stability", "stable").c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 23, block_json.value("is_active", true) ? 1 : 0);

            sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            // ISS-013 Fix: Insert into FTS5 table for full-text search
            std::string keywords_str;
            if (block_json.contains("keywords") && block_json["keywords"].is_array()) {
                for (const auto& keyword : block_json["keywords"]) {
                    if (!keywords_str.empty()) keywords_str += " ";
                    keywords_str += keyword.get<std::string>();
                }
            }

            std::string use_cases_str;
            if (block_json.contains("use_cases") && block_json["use_cases"].is_array()) {
                for (const auto& use_case : block_json["use_cases"]) {
                    if (!use_cases_str.empty()) use_cases_str += " ";
                    use_cases_str += use_case.get<std::string>();
                }
            }

            const char* fts_sql = R"(
                INSERT INTO blocks_fts (block_id, name, description, short_desc, keywords, use_cases)
                VALUES (?, ?, ?, ?, ?, ?)
            )";

            sqlite3_stmt* fts_stmt;
            if (sqlite3_prepare_v2(db_, fts_sql, -1, &fts_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(fts_stmt, 1, block_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(fts_stmt, 2, block_json.value("name", block_id).c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(fts_stmt, 3, block_json.value("description", "").c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(fts_stmt, 4, block_json.value("short_desc", "").c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(fts_stmt, 5, keywords_str.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(fts_stmt, 6, use_cases_str.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(fts_stmt);
                sqlite3_finalize(fts_stmt);
            }

            // Index keywords
            if (block_json.contains("keywords") && block_json["keywords"].is_array()) {
                for (const auto& keyword : block_json["keywords"]) {
                    insertKeyword(block_id, keyword.get<std::string>());
                }
            }

            // Index use cases
            if (block_json.contains("use_cases") && block_json["use_cases"].is_array()) {
                for (const auto& use_case : block_json["use_cases"]) {
                    insertUseCase(block_id, use_case.get<std::string>());
                }
            }

            return true;

        } catch (const std::exception& e) {
            return false;
        }
    }

    void insertKeyword(const std::string& block_id, const std::string& keyword) {
        const char* sql = "INSERT INTO block_keywords (block_id, keyword) VALUES (?, ?)";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, block_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, keyword.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void insertUseCase(const std::string& block_id, const std::string& use_case) {
        const char* sql = "INSERT INTO block_use_cases (block_id, use_case) VALUES (?, ?)";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, block_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, use_case.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    int getBlockCount() const {
        const char* sql = "SELECT COUNT(*) FROM blocks WHERE is_active = 1";
        sqlite3_stmt* stmt;
        int count = 0;

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }

        return count;
    }
};

// BlockSearchIndex implementation
BlockSearchIndex::BlockSearchIndex(const std::string& db_path)
    : pimpl_(std::make_unique<Impl>(db_path)) {
}

BlockSearchIndex::~BlockSearchIndex() = default;

int BlockSearchIndex::buildIndex(const std::string& blocks_path) {
    return pimpl_->buildIndex(blocks_path);
}

int BlockSearchIndex::getBlockCount() const {
    return pimpl_->getBlockCount();
}

std::vector<SearchResult> BlockSearchIndex::search(const SearchQuery& query) {
    std::vector<SearchResult> results;

    // Build SQL query with filters
    std::stringstream sql;
    sql << "SELECT b.* FROM blocks b ";

    bool use_fts = !query.query.empty();
    if (use_fts) {
        sql << "JOIN blocks_fts f ON b.block_id = f.block_id ";
        sql << "WHERE f.blocks_fts MATCH ? ";
    } else {
        sql << "WHERE 1=1 ";
    }

    // Add filters
    if (query.language.has_value()) {
        sql << "AND b.language = ? ";
    }
    if (query.category.has_value()) {
        sql << "AND b.category = ? ";
    }
    if (query.performance_tier.has_value()) {
        sql << "AND b.performance_tier = ? ";
    }
    if (query.min_success_rate > 0) {
        sql << "AND b.success_rate_percent >= ? ";
    }

    sql << "AND b.is_active = 1 ";
    sql << "ORDER BY b.times_used DESC, b.success_rate_percent DESC ";
    sql << "LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(pimpl_->db_, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    // Bind parameters
    int param_idx = 1;
    if (use_fts) {
        sqlite3_bind_text(stmt, param_idx++, query.query.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (query.language.has_value()) {
        sqlite3_bind_text(stmt, param_idx++, query.language->c_str(), -1, SQLITE_TRANSIENT);
    }
    if (query.category.has_value()) {
        sqlite3_bind_text(stmt, param_idx++, query.category->c_str(), -1, SQLITE_TRANSIENT);
    }
    if (query.performance_tier.has_value()) {
        sqlite3_bind_text(stmt, param_idx++, query.performance_tier->c_str(), -1, SQLITE_TRANSIENT);
    }
    if (query.min_success_rate > 0) {
        sqlite3_bind_int(stmt, param_idx++, query.min_success_rate);
    }
    sqlite3_bind_int(stmt, param_idx++, query.limit);
    sqlite3_bind_int(stmt, param_idx++, query.offset);

    // Execute query and build results
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SearchResult result;

        // Parse BlockMetadata
        result.metadata.block_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result.metadata.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        result.metadata.language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        const char* cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result.metadata.category = cat ? cat : "";
        const char* subcat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result.metadata.subcategory = subcat ? subcat : "";

        result.metadata.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        result.metadata.code_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        result.metadata.token_count = sqlite3_column_int(stmt, 7);
        result.metadata.times_used = sqlite3_column_int(stmt, 8);
        result.metadata.version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));

        // AI discovery fields
        const char* desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        result.metadata.description = desc ? desc : "";
        const char* short_desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        result.metadata.short_desc = short_desc ? short_desc : "";
        const char* input_types = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        result.metadata.input_types = input_types ? input_types : "";
        const char* output_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        result.metadata.output_type = output_type ? output_type : "";

        // Performance metrics
        result.metadata.avg_execution_ms = sqlite3_column_double(stmt, 14);
        result.metadata.max_memory_mb = sqlite3_column_int(stmt, 15);
        const char* perf_tier = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
        result.metadata.performance_tier = perf_tier ? perf_tier : "unknown";
        result.metadata.success_rate_percent = sqlite3_column_int(stmt, 17);
        result.metadata.avg_tokens_saved = sqlite3_column_int(stmt, 18);

        // Quality metrics
        result.metadata.test_coverage_percent = sqlite3_column_int(stmt, 19);
        result.metadata.security_audited = sqlite3_column_int(stmt, 20) != 0;
        const char* stability = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 21));
        result.metadata.stability = stability ? stability : "stable";
        result.metadata.is_active = sqlite3_column_int(stmt, 22) != 0;

        // Calculate scores
        result.relevance_score = use_fts ? 1.0 : 0.5;  // FTS provides relevance
        result.popularity_score = std::min(1.0, result.metadata.times_used / 100.0);
        result.quality_score = result.metadata.success_rate_percent / 100.0;

        // Weighted final score: 50% relevance, 30% quality, 20% popularity
        result.final_score = (result.relevance_score * 0.5) +
                            (result.quality_score * 0.3) +
                            (result.popularity_score * 0.2);

        // Generate snippet from description
        result.snippet = result.metadata.short_desc.empty()
            ? result.metadata.description.substr(0, 100)
            : result.metadata.short_desc;

        results.push_back(result);
    }

    sqlite3_finalize(stmt);

    // Sort by final score
    std::sort(results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b) {
            return a.final_score > b.final_score;
        });

    return results;
}

std::optional<BlockMetadata> BlockSearchIndex::getBlock(const std::string& block_id) {
    const char* sql = "SELECT * FROM blocks WHERE block_id = ? LIMIT 1";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, block_id.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    // Parse BlockMetadata
    BlockMetadata metadata;
    metadata.block_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    metadata.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    metadata.language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

    const char* cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    metadata.category = cat ? cat : "";
    const char* subcat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    metadata.subcategory = subcat ? subcat : "";

    metadata.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    metadata.code_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    metadata.token_count = sqlite3_column_int(stmt, 7);
    metadata.times_used = sqlite3_column_int(stmt, 8);
    metadata.version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));

    // AI discovery fields
    const char* desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
    metadata.description = desc ? desc : "";
    const char* short_desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    metadata.short_desc = short_desc ? short_desc : "";
    const char* input_types = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
    metadata.input_types = input_types ? input_types : "";
    const char* output_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
    metadata.output_type = output_type ? output_type : "";

    // Performance metrics
    metadata.avg_execution_ms = sqlite3_column_double(stmt, 14);
    metadata.max_memory_mb = sqlite3_column_int(stmt, 15);
    const char* perf_tier = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
    metadata.performance_tier = perf_tier ? perf_tier : "unknown";
    metadata.success_rate_percent = sqlite3_column_int(stmt, 17);
    metadata.avg_tokens_saved = sqlite3_column_int(stmt, 18);

    // Quality metrics
    metadata.test_coverage_percent = sqlite3_column_int(stmt, 19);
    metadata.security_audited = sqlite3_column_int(stmt, 20) != 0;
    const char* stability = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 21));
    metadata.stability = stability ? stability : "stable";
    metadata.is_active = sqlite3_column_int(stmt, 22) != 0;

    sqlite3_finalize(stmt);
    return metadata;
}

std::map<std::string, int> BlockSearchIndex::getStatistics() const {
    std::map<std::string, int> stats;
    stats["total_blocks"] = getBlockCount();
    return stats;
}

void BlockSearchIndex::recordUsage(const std::string& block_id) {
    const char* sql = "UPDATE blocks SET times_used = times_used + 1 WHERE block_id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(pimpl_->db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, block_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void BlockSearchIndex::clearIndex() {
    const char* sql = "DELETE FROM blocks; DELETE FROM block_keywords; DELETE FROM block_use_cases;";
    sqlite3_exec(pimpl_->db_, sql, nullptr, nullptr, nullptr);
}

} // namespace runtime
} // namespace naab
