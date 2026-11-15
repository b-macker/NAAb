// The block library: BlockRegistry (metadata and source, behind `naab-lang
// blocks info` and `use BLOCK-...`) and BlockSearchIndex (behind `naab-lang
// blocks search`).
//
// Folded in from tests/unit/test_enhanced_metadata.cpp and
// tests/unit/test_search_index.cpp, standalone programs in the default build
// target that nothing ran (A58). Both failed at their first check for one
// reason: both components have read blocks from <dir>/<language>/<id>.json
// since the first commit (5d49859c), and the fixture sat flat in
// tests/fixtures/block-samples/. It now sits in a javascript/ folder, and its
// code_hash values are the real SHA-256 of each block's code -- the made-up
// ones made the registry's source integrity check (V-RT-004) refuse the
// source, which is correct, and which RegistryRefusesASourceChangedAfterRegistration
// now asserts on purpose. Measuring the search this way found A170: a text
// query finds nothing (pinned below).
//
// Each test works on a COPY of the fixture in a temporary directory: the
// registry writes .block_cache.json beside the blocks it scans, and the
// search index opens a database, and neither may land in the repository.
#include "naab/audit_logger.h"
#include "naab/block_registry.h"
#include "naab/block_search_index.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using naab::runtime::BlockRegistry;
using naab::runtime::BlockSearchIndex;
using naab::runtime::SearchQuery;

namespace {

// A fresh directory holding a copy of tests/fixtures/block-samples (the unit
// runner starts in the repository root, as other tests here rely on).
fs::path copyFixtures(const std::string& tag) {
    static std::atomic<int> n{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
                   ("naab_blocks_" + tag + "_" + std::to_string(stamp) + "_" + std::to_string(n++));
    fs::create_directories(dir);
    fs::copy("tests/fixtures/block-samples", dir, fs::copy_options::recursive);
    return dir;
}

// Removes a test's temporary copy when the test ends, however it ends.
struct RemoveOnExit {
    fs::path dir;
    ~RemoveOnExit() {
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
};

std::string readAll(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

// The registry is a process-wide singleton that initialises once and cannot be
// reset, and the interpreter initialises it lazily from ~/.naab when a program
// `use`s a block. This file is listed FIRST in naab_unit_tests so its tests run
// before any other; if that ever stops being true this fails rather than
// measuring a registry another test filled.
TEST(BlockRegistryTest, ReadsEnhancedMetadataFromALanguageFolder) {
    auto& registry = BlockRegistry::instance();
    ASSERT_FALSE(registry.isInitialized())
        << "the registry singleton was initialised before this test ran; keep "
           "block_library_test.cpp first in naab_unit_tests";
    const fs::path dir = copyFixtures("registry");
    registry.initialize(dir.string());
    ASSERT_TRUE(registry.isInitialized());

    auto block_opt = registry.getBlock("TEST-ENHANCED-META");
    ASSERT_TRUE(block_opt.has_value()) << "the fixture's block should be found under javascript/";
    const auto& block = *block_opt;
    EXPECT_EQ(block.block_id, "TEST-ENHANCED-META");
    EXPECT_EQ(block.name, "Test Enhanced Metadata Block");
    EXPECT_EQ(block.version, "1.2.3");
    EXPECT_EQ(block.language, "javascript");

    EXPECT_NE(block.description.find("comprehensive test block"), std::string::npos);
    EXPECT_EQ(block.short_desc, "Test block for enhanced metadata");
    EXPECT_EQ(block.input_types, "string, number");
    EXPECT_EQ(block.output_type, "object");

    ASSERT_EQ(block.keywords.size(), 4u);
    EXPECT_EQ(block.keywords[0], "test");
    EXPECT_EQ(block.keywords[1], "metadata");
    EXPECT_EQ(block.keywords[2], "enhanced");
    EXPECT_EQ(block.keywords[3], "ai-discovery");

    ASSERT_EQ(block.use_cases.size(), 3u);
    EXPECT_EQ(block.use_cases[0], "Testing metadata parsing");
    EXPECT_EQ(block.use_cases[1], "Validating JSON field extraction");
    EXPECT_EQ(block.use_cases[2], "AI-powered block discovery");

    ASSERT_EQ(block.related_blocks.size(), 2u);
    EXPECT_EQ(block.related_blocks[0], "TEST-BASIC-META");
    EXPECT_EQ(block.related_blocks[1], "TEST-SIMPLE-META");

    EXPECT_NEAR(block.avg_execution_ms, 12.5, 0.01);
    EXPECT_EQ(block.max_memory_mb, 8);
    EXPECT_EQ(block.performance_tier, "fast");
    EXPECT_EQ(block.success_rate_percent, 98);
    EXPECT_EQ(block.avg_tokens_saved, 150);

    EXPECT_EQ(block.test_coverage_percent, 95);
    EXPECT_TRUE(block.security_audited);
    EXPECT_EQ(block.stability, "stable");

    EXPECT_EQ(registry.blockCount(), 2u);

    // The source reads back, through the source integrity check (V-RT-004):
    // the fixture's code_hash is the real hash of its code.
    const std::string source = registry.getBlockSource("TEST-ENHANCED-META");
    EXPECT_NE(source.find("Enhanced metadata test"), std::string::npos);

    // Nothing was written into the repository's fixture.
    EXPECT_FALSE(fs::exists("tests/fixtures/block-samples/.block_cache.json"));
}

// A block whose code changed on disk after it was registered is refused when
// its source is read, and the mismatch is logged. Runs on the registry the
// test above initialised (a singleton); TEST-BASIC-BLOCK's source has not
// been read yet, so it is read from the file now.
TEST(BlockRegistryTest, RegistryRefusesASourceChangedAfterRegistration) {
    auto& registry = BlockRegistry::instance();
    ASSERT_TRUE(registry.isInitialized()) << "runs after ReadsEnhancedMetadataFromALanguageFolder";
    auto block_opt = registry.getBlock("TEST-BASIC-BLOCK");
    ASSERT_TRUE(block_opt.has_value());
    const fs::path file = block_opt->file_path;
    ASSERT_NE(file.string().find("naab_blocks_registry_"), std::string::npos)
        << "the registry should be reading the temporary copy, not the repository";
    // The copy the test above made; nothing reads it after this test.
    RemoveOnExit cleanup{file.parent_path().parent_path()};

    std::string text = readAll(file);
    const std::string from = "return JSON.parse(data);";
    const auto at = text.find(from);
    ASSERT_NE(at, std::string::npos);
    text.replace(at, from.size(), "return JSON.parse(data) || 1;");
    { std::ofstream(file, std::ios::trunc) << text; }

    const fs::path log = file.parent_path() / "security.log";
    naab::security::AuditLogger::setLogFile(log.string());
    EXPECT_THROW(registry.getBlockSource("TEST-BASIC-BLOCK"), std::runtime_error);
    naab::security::AuditLogger::flush();
    naab::security::AuditLogger::setLogFile("");
    EXPECT_NE(readAll(log).find("TEST-BASIC-BLOCK"), std::string::npos)
        << "the refused source should be logged as a hash mismatch";
}

TEST(BlockSearchIndexTest, IndexesTheFixtureAndSearchesIt) {
    const fs::path dir = copyFixtures("search");
    RemoveOnExit cleanup{dir};
    BlockSearchIndex index((dir / "index.db").string());
    const int indexed = index.buildIndex(dir.string());
    ASSERT_EQ(indexed, 2) << "both fixture blocks under javascript/ should be indexed";
    EXPECT_EQ(index.getBlockCount(), 2);

    auto block_opt = index.getBlock("TEST-ENHANCED-META");
    ASSERT_TRUE(block_opt.has_value());
    EXPECT_EQ(block_opt->name, "Test Enhanced Metadata Block");
    EXPECT_FALSE(block_opt->description.empty());

    SearchQuery all;
    all.query = "";
    all.limit = 10;
    const auto every = index.search(all);
    EXPECT_EQ(every.size(), 2u);

    // PINNED, not endorsed (A170): a TEXT query finds nothing. The full-text
    // table is declared with content='blocks' and columns (keywords,
    // use_cases) the blocks table lacks, so every query that reads it fails
    // inside SQLite and search() returns an empty list -- `naab-lang blocks
    // search metadata` answers "No blocks found" for these two blocks
    // (observed). When A170 is fixed this fails: assert the match instead.
    SearchQuery text;
    text.query = "metadata";
    text.limit = 10;
    EXPECT_TRUE(index.search(text).empty())
        << "A170 looks fixed: a text search found blocks. Confirm, then assert that "
           "'metadata' finds TEST-ENHANCED-META here.";

    SearchQuery js;
    js.query = "";
    js.language = "javascript";
    js.limit = 10;
    const auto in_js = index.search(js);
    EXPECT_EQ(in_js.size(), 2u);
    for (const auto& r : in_js) EXPECT_EQ(r.metadata.language, "javascript");

    SearchQuery other;
    other.query = "";
    other.language = "python";
    other.limit = 10;
    EXPECT_TRUE(index.search(other).empty()) << "the language filter should exclude every fixture block";

    auto stats = index.getStatistics();
    EXPECT_EQ(stats["total_blocks"], 2);

    const int before = block_opt->times_used;
    index.recordUsage("TEST-ENHANCED-META");
    auto after = index.getBlock("TEST-ENHANCED-META");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->times_used, before + 1);
}
