#pragma once

// NAAb Project Context Awareness — Governance as a Living Partner
//
// Makes NAAb governance aware of the full project context by extracting rules
// from three source layers:
//   1. LLM instruction files (CLAUDE.md, .cursorrules, copilot-instructions.md)
//   2. Linter/formatter configs (.editorconfig, .eslintrc, .prettierrc)
//   3. Package manifests (package.json, go.mod, Cargo.toml)
//
// govern.json always overrides everything. Project context supplements only.
// Opt-in, off by default. Each layer independently toggleable.

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace naab {
namespace governance {

// Forward declarations
struct GovernanceRules;
struct PolyglotOptimizationConfig;
enum class EnforcementLevel;

// ============================================================================
// Enums & Structs
// ============================================================================

enum class ContextLayer {
    LLM,        // Layer 1: LLM instruction files
    LINTER,     // Layer 2: Linter/formatter configs
    MANIFEST    // Layer 3: Package manifests
};

struct ContextExtraction {
    std::string id;              // Deterministic rule ID (for suppress_rules)
    ContextLayer layer;          // Which layer this came from
    std::string source_file;     // Filename (e.g., "CLAUDE.md", ".eslintrc.json")
    std::string source_path;     // Full path
    int line_number = 0;         // 0 for structured configs
    std::string category;        // "language", "banned", "style", "custom", "manifest"
    std::string directive;       // Extracted rule text
    std::string mapped_rule;     // What governance field it maps to
    std::string original_line;   // Raw line/key from source
    std::string status;          // "applied", "skipped:conflict", "skipped:govern", "skipped:suppressed", "redundant", "dry_run"
    std::string skip_reason;     // Human-readable reason if skipped
    int confidence = 100;        // 100 for linters, 80 for LLM, 60 for manifests

    // For language preference extractions
    std::string language;        // Normalized language name
    std::string task;            // Task category (if task-specific)
    int score_boost = 0;         // Optimization score boost
};

// ============================================================================
// Configuration (parsed from govern.json "project_context" section)
// ============================================================================

struct ProjectContextSourceConfig {
    bool llm = true;
    bool linters = true;
    bool manifests = true;
};

struct ProjectContextConfig {
    bool enabled = false;
    std::string enforcement_level = "advisory";
    std::string priority_source;              // empty = layer-based priority
    ProjectContextSourceConfig sources;
    std::vector<std::string> watch_files;
    std::vector<std::string> ignore_files;
    std::vector<std::string> suppress_rules;
    bool extract_language_prefs = true;
    bool extract_banned_patterns = true;
    bool extract_style_rules = true;
    bool extract_custom_directives = true;
    bool feed_optimization = true;
    bool show_extractions = true;
    bool dry_run = false;
    int max_file_size_kb = 100;
};

// ============================================================================
// ProjectContextLoader — Main class
// ============================================================================

class ProjectContextLoader {
public:
    // Main entry point: discover files across all layers, parse, extract
    std::vector<ContextExtraction> loadContext(
        const std::string& project_dir,
        const ProjectContextConfig& config);

    // Apply extractions to governance rules (supplement, not override)
    void applyToRules(GovernanceRules& rules,
                      std::vector<ContextExtraction>& extractions,
                      EnforcementLevel level);

    // Apply optimization hints to scoring matrix
    void applyOptimizationHints(PolyglotOptimizationConfig& opt_config,
                                const std::vector<ContextExtraction>& extractions);

    // Format extraction report for stderr
    std::string formatReport(const std::vector<ContextExtraction>& extractions,
                             bool verbose = false);

private:
    // Layer 1: LLM files
    std::vector<ContextExtraction> discoverAndParseLLMFiles(
        const std::string& dir, const ProjectContextConfig& config);
    std::vector<ContextExtraction> parseMarkdownFile(
        const std::string& path, const std::string& filename);
    void extractFromLine(const std::string& line, int line_num,
                         const std::string& source_file, const std::string& source_path,
                         const std::string& section,
                         std::vector<ContextExtraction>& out);

    // Layer 2: Linter configs
    std::vector<ContextExtraction> discoverAndParseLinterConfigs(
        const std::string& dir, const ProjectContextConfig& config);
    std::vector<ContextExtraction> parseEditorConfig(const std::string& path);
    std::vector<ContextExtraction> parseJsonConfig(
        const std::string& path, const std::string& filename);

    // Layer 3: Manifests
    std::vector<ContextExtraction> discoverAndParseManifests(
        const std::string& dir, const ProjectContextConfig& config);
    std::vector<ContextExtraction> parseManifestFile(
        const std::string& path, const std::string& filename,
        const std::string& language);

    // Helpers
    std::string normalizeLanguage(const std::string& name);
    std::string normalizeTask(const std::string& task);
    std::string generateRuleId(ContextLayer layer, const std::string& file,
                               const std::string& category, const std::string& key);
    std::string stripMarkdownFormatting(const std::string& line);
    std::string inferLanguageFromFunction(const std::string& func_name);

    // File walking (shared across layers)
    std::vector<std::string> walkUpward(const std::string& start_dir,
                                         const std::vector<std::string>& filenames,
                                         const ProjectContextConfig& config);

    // Conflict resolution
    void resolveConflicts(std::vector<ContextExtraction>& extractions,
                          const ProjectContextConfig& config);
};

} // namespace governance
} // namespace naab
