#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace naab {
namespace scanner {

// Single issue found by a check
struct Issue {
    std::string file;
    int line = 0;
    std::string rule;        // e.g., "obvious_comments"
    std::string category;    // e.g., "redundancy"
    std::string level;       // "hard", "soft", "advisory"
    std::string severity;    // "critical", "high", "medium", "low"
    std::string message;
    std::string preview;     // source line preview (truncated to 120 chars)
    std::string fix;         // suggested fix
};

// Per-check config (opaque — loaded from JSON in .cpp)
struct CheckConfig {
    bool enabled = true;
    std::string level = "soft";
    // Options stored as key-value pairs for simple access
    std::unordered_map<std::string, double> num_options;
    std::unordered_map<std::string, std::string> str_options;
    std::vector<std::string> list_options;  // for "names" lists etc.
};

// Scanner configuration (loaded from govern.json "scanner" section)
struct ScanConfig {
    std::string version = "1.0";
    std::string mode = "enforce";

    // scan settings
    int max_files = 200;
    int max_file_size_kb = 500;
    std::vector<std::string> exclude_patterns;
    bool include_tests = false;
    bool follow_symlinks = false;

    // check configs by "category.check_id"
    std::unordered_map<std::string, CheckConfig> checks;

    // output settings
    std::string output_format = "text";
    int max_issues_per_file = 50;
    int max_total_issues = 500;
    std::string group_by = "file";
    std::string sort_by = "severity";
    bool show_line_preview = true;
    bool show_fix_suggestion = true;
    bool save_json = true;
    bool save_text = true;
    bool save_sarif = false;
    std::string json_path = "quality-report.json";
    std::string text_path = "quality-report.txt";
    std::string sarif_path = "quality-report.sarif";
};

// Scan result
struct ScanResult {
    std::string path;
    std::string language;
    int files_scanned = 0;
    std::vector<Issue> issues;
};

// Main engine
class ScannerEngine {
public:
    // Load config from govern.json scanner section (discovered from target path)
    bool loadConfig(const std::string& target_path);

    // Load config from a known govern.json path (for runtime integration — no discovery needed)
    bool loadConfigFromPath(const std::string& govern_json_path, bool quiet = false);

    // Check if scanner section was found in govern.json
    bool hasConfig() const { return has_scanner_config_; }

    // Run scan
    ScanResult scan(const std::string& target_path, const std::string& language);

    // Report generation
    std::string formatTextReport(const ScanResult& result) const;
    std::string formatJsonReport(const ScanResult& result) const;
    void saveReports(const ScanResult& result) const;

    // Get config (for external access)
    const ScanConfig& config() const { return config_; }

    // Config helpers (public for check modules)
    bool isEnabled(const std::string& category, const std::string& check_id) const;
    std::string getLevel(const std::string& category, const std::string& check_id) const;
    std::string levelToSeverity(const std::string& level) const;
    double getNumOption(const std::string& category, const std::string& check_id,
                        const std::string& key, double default_val) const;
    std::string getStrOption(const std::string& category, const std::string& check_id,
                             const std::string& key, const std::string& default_val) const;
    std::vector<std::string> getListOption(const std::string& category, const std::string& check_id) const;

    // Issue helper
    void addIssue(std::vector<Issue>& issues,
                  const std::string& filepath, int line,
                  const std::string& rule, const std::string& category,
                  const std::string& message, const std::string& preview,
                  const std::string& fix,
                  const std::string& level_override = "") const;

    // NAAb polyglot stripping (shared utility)
    static std::vector<std::string> stripPolyglotBlocks(const std::vector<std::string>& lines);

    // Shared utility: find function end
    static size_t findFuncEnd(const std::vector<std::string>& lines,
                              size_t start_line, int base_indent,
                              const std::string& language);

private:
    ScanConfig config_;
    std::string govern_path_;
    bool has_scanner_config_ = false;

    // File discovery
    std::vector<std::string> collectFiles(const std::string& target_path,
                                           const std::string& language) const;
    static std::string detectLanguage(const std::string& filepath);
    static std::string discoverGovernJson(const std::string& start_path);

    // Scanning
    std::vector<Issue> scanFile(const std::string& filepath,
                                 const std::string& language) const;

    // Check modules (each adds issues to the vector)
    void checkRedundancy(const std::string& filepath,
                         const std::vector<std::string>& lines,
                         const std::string& language,
                         std::vector<Issue>& issues) const;
    void checkCodeQuality(const std::string& filepath,
                          const std::vector<std::string>& lines,
                          const std::string& language,
                          std::vector<Issue>& issues) const;
    void checkComplexity(const std::string& filepath,
                         const std::vector<std::string>& lines,
                         const std::string& language,
                         std::vector<Issue>& issues) const;
    void checkStyle(const std::string& filepath,
                    const std::vector<std::string>& lines,
                    const std::string& content,
                    const std::string& language,
                    std::vector<Issue>& issues) const;
    void checkSecurity(const std::string& filepath,
                       const std::vector<std::string>& lines,
                       const std::string& language,
                       std::vector<Issue>& issues) const;
    void checkLangPython(const std::string& filepath,
                         const std::vector<std::string>& lines,
                         const std::string& content,
                         std::vector<Issue>& issues) const;
    void checkLangJavascript(const std::string& filepath,
                             const std::vector<std::string>& lines,
                             std::vector<Issue>& issues) const;
    void checkLangCpp(const std::string& filepath,
                      const std::vector<std::string>& lines,
                      std::vector<Issue>& issues) const;
    void checkLangGo(const std::string& filepath,
                     const std::vector<std::string>& lines,
                     std::vector<Issue>& issues) const;
    void checkLangRust(const std::string& filepath,
                       const std::vector<std::string>& lines,
                       std::vector<Issue>& issues) const;
    void checkLangNaab(const std::string& filepath,
                       const std::vector<std::string>& lines,
                       const std::vector<std::string>& orig_lines,
                       const std::string& content,
                       std::vector<Issue>& issues) const;
};

} // namespace scanner
} // namespace naab
