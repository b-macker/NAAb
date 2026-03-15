// NAAb Scanner Engine — Config, file discovery, report generation
// Pure C++ implementation (no Python dependency)

#include "naab/scanner.h"
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <set>
#include <map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace naab {
namespace scanner {

// ============================================================================
// Utility functions
// ============================================================================

static std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> ScannerEngine::stripPolyglotBlocks(
    const std::vector<std::string>& lines) {
    std::vector<std::string> result = lines;
    static const std::regex poly_open(R"(<<\w+)");
    static const std::regex poly_close(R"(^>>)");
    bool in_poly = false;
    for (size_t i = 0; i < result.size(); ++i) {
        std::string stripped = result[i];
        // Trim leading whitespace for checking
        size_t first_nonspace = stripped.find_first_not_of(" \t");
        std::string trimmed = (first_nonspace != std::string::npos) ?
                              stripped.substr(first_nonspace) : "";

        if (!in_poly && stripped.find("<<") != std::string::npos &&
            std::regex_search(stripped, poly_open)) {
            in_poly = true;
            result[i] = "";
        } else if (in_poly && std::regex_match(result[i], poly_close)) {
            in_poly = false;
            result[i] = "";
        } else if (in_poly) {
            result[i] = "";
        }
    }
    return result;
}

size_t ScannerEngine::findFuncEnd(const std::vector<std::string>& lines,
                                   size_t start_line, int base_indent,
                                   const std::string& language) {
    if (language == "python") {
        size_t e = start_line + 1;
        while (e < lines.size()) {
            const auto& el = lines[e];
            std::string stripped = el;
            size_t first = stripped.find_first_not_of(" \t");
            std::string trimmed = (first != std::string::npos) ? stripped.substr(first) : "";
            if (trimmed.empty() || trimmed[0] == '@' || trimmed[0] == '#') {
                e++;
                continue;
            }
            int cur_ind = (first != std::string::npos) ? static_cast<int>(first) : 0;
            if (cur_ind <= base_indent) break;
            e++;
        }
        return e;
    } else {
        int brace_depth = 0;
        bool found_open = false;
        for (size_t j = start_line; j < lines.size(); ++j) {
            for (char c : lines[j]) {
                if (c == '{') { brace_depth++; found_open = true; }
                if (c == '}') { brace_depth--; }
            }
            if (found_open && brace_depth <= 0) {
                return j + 1;
            }
        }
        return lines.size();
    }
}

// ============================================================================
// Config helpers
// ============================================================================

bool ScannerEngine::isEnabled(const std::string& category,
                               const std::string& check_id) const {
    auto key = category + "." + check_id;
    auto it = config_.checks.find(key);
    if (it == config_.checks.end()) return true;
    return it->second.enabled;
}

std::string ScannerEngine::getLevel(const std::string& category,
                                     const std::string& check_id) const {
    auto key = category + "." + check_id;
    auto it = config_.checks.find(key);
    if (it == config_.checks.end()) return "soft";
    return it->second.level;
}

std::string ScannerEngine::levelToSeverity(const std::string& level) const {
    if (level == "hard") return "high";
    if (level == "soft") return "medium";
    if (level == "advisory") return "low";
    return "medium";
}

double ScannerEngine::getNumOption(const std::string& category,
                                    const std::string& check_id,
                                    const std::string& key,
                                    double default_val) const {
    auto cfg_key = category + "." + check_id;
    auto it = config_.checks.find(cfg_key);
    if (it == config_.checks.end()) return default_val;
    auto opt_it = it->second.num_options.find(key);
    if (opt_it == it->second.num_options.end()) return default_val;
    return opt_it->second;
}

std::string ScannerEngine::getStrOption(const std::string& category,
                                         const std::string& check_id,
                                         const std::string& key,
                                         const std::string& default_val) const {
    auto cfg_key = category + "." + check_id;
    auto it = config_.checks.find(cfg_key);
    if (it == config_.checks.end()) return default_val;
    auto opt_it = it->second.str_options.find(key);
    if (opt_it == it->second.str_options.end()) return default_val;
    return opt_it->second;
}

std::vector<std::string> ScannerEngine::getListOption(
    const std::string& category, const std::string& check_id) const {
    auto cfg_key = category + "." + check_id;
    auto it = config_.checks.find(cfg_key);
    if (it == config_.checks.end()) return {};
    return it->second.list_options;
}

void ScannerEngine::addIssue(std::vector<Issue>& issues,
                              const std::string& filepath, int line,
                              const std::string& rule, const std::string& category,
                              const std::string& message, const std::string& preview,
                              const std::string& fix,
                              const std::string& level_override) const {
    std::string lvl = level_override.empty() ? getLevel(category, rule) : level_override;
    Issue issue;
    issue.file = filepath;
    issue.line = line;
    issue.rule = rule;
    issue.category = category;
    issue.level = lvl;
    issue.severity = levelToSeverity(lvl);
    issue.message = message;
    issue.preview = preview.substr(0, 120);
    issue.fix = fix;
    issues.push_back(std::move(issue));
}

// ============================================================================
// Language detection
// ============================================================================

std::string ScannerEngine::detectLanguage(const std::string& filepath) {
    fs::path p(filepath);
    std::string ext = p.extension().string();
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".py") return "python";
    if (ext == ".js" || ext == ".jsx" || ext == ".ts" || ext == ".tsx") return "javascript";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp") return "cpp";
    if (ext == ".go") return "go";
    if (ext == ".rs") return "rust";
    if (ext == ".naab") return "naab";
    return "unknown";
}

// ============================================================================
// govern.json discovery
// ============================================================================

std::string ScannerEngine::discoverGovernJson(const std::string& start_path) {
    std::error_code ec;
    fs::path p = fs::absolute(start_path, ec);
    if (ec) return "";

    fs::path dir_path;
    if (fs::is_regular_file(p, ec)) {
        dir_path = p.parent_path();
    } else {
        dir_path = p;
    }

    while (true) {
        fs::path candidate = dir_path / "govern.json";
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
        fs::path parent = dir_path.parent_path();
        if (parent == dir_path) break;
        dir_path = parent;
    }
    return "";
}

// ============================================================================
// Config loading
// ============================================================================

static void loadCheckConfigs(const json& category_json,
                              const std::string& category_name,
                              std::unordered_map<std::string, CheckConfig>& checks) {
    if (!category_json.is_object()) return;
    for (auto& [check_id, check_val] : category_json.items()) {
        if (!check_val.is_object()) continue;
        CheckConfig cc;
        if (check_val.contains("enabled")) {
            cc.enabled = check_val["enabled"].get<bool>();
        }
        if (check_val.contains("level")) {
            cc.level = check_val["level"].get<std::string>();
        }
        // Load numeric options
        for (auto& [key, val] : check_val.items()) {
            if (key == "enabled" || key == "level") continue;
            if (val.is_number()) {
                cc.num_options[key] = val.get<double>();
            } else if (val.is_string()) {
                cc.str_options[key] = val.get<std::string>();
            } else if (val.is_array()) {
                for (auto& elem : val) {
                    if (elem.is_string()) {
                        cc.list_options.push_back(elem.get<std::string>());
                    }
                }
            }
        }
        checks[category_name + "." + check_id] = std::move(cc);
    }
}

bool ScannerEngine::loadConfigFromPath(const std::string& govern_json_path, bool quiet) {
    govern_path_ = govern_json_path;
    has_scanner_config_ = false;

    std::ifstream file(govern_path_);
    if (!file.is_open()) {
        if (!quiet) fmt::print("Warning: Could not read {}\n", govern_path_);
        return false;
    }

    json full_cfg;
    try {
        file >> full_cfg;
    } catch (const json::parse_error& e) {
        if (!quiet) fmt::print("Warning: Invalid JSON in {}\n", govern_path_);
        return false;
    }

    if (!full_cfg.contains("scanner") || !full_cfg["scanner"].is_object()) {
        if (!quiet) fmt::print("Note: No 'scanner' section in {}. Using defaults.\n", govern_path_);
        return false;
    }

    has_scanner_config_ = true;
    auto& scanner_cfg = full_cfg["scanner"];
    if (!quiet) fmt::print("Config: {}\n", govern_path_);

    // Load version/mode
    if (scanner_cfg.contains("version")) config_.version = scanner_cfg["version"].get<std::string>();
    if (scanner_cfg.contains("mode")) config_.mode = scanner_cfg["mode"].get<std::string>();

    // Load scan settings
    if (scanner_cfg.contains("scan") && scanner_cfg["scan"].is_object()) {
        auto& scan = scanner_cfg["scan"];
        if (scan.contains("max_files")) config_.max_files = scan["max_files"].get<int>();
        if (scan.contains("max_file_size_kb")) config_.max_file_size_kb = scan["max_file_size_kb"].get<int>();
        if (scan.contains("include_tests")) config_.include_tests = scan["include_tests"].get<bool>();
        if (scan.contains("follow_symlinks")) config_.follow_symlinks = scan["follow_symlinks"].get<bool>();
        if (scan.contains("exclude_patterns") && scan["exclude_patterns"].is_array()) {
            config_.exclude_patterns.clear();
            for (auto& p : scan["exclude_patterns"]) {
                config_.exclude_patterns.push_back(p.get<std::string>());
            }
        }
    }

    // Load check configs for each category
    const std::vector<std::string> categories = {
        "redundancy", "code_quality", "complexity", "style", "security"
    };
    for (const auto& cat : categories) {
        if (scanner_cfg.contains(cat)) {
            loadCheckConfigs(scanner_cfg[cat], cat, config_.checks);
        }
    }

    // Load language-specific check configs
    if (scanner_cfg.contains("lang_rules") && scanner_cfg["lang_rules"].is_object()) {
        auto& lang_rules = scanner_cfg["lang_rules"];
        const std::vector<std::string> langs = {
            "python", "javascript", "cpp", "go", "rust", "naab"
        };
        for (const auto& lang : langs) {
            if (lang_rules.contains(lang)) {
                loadCheckConfigs(lang_rules[lang], "lang_" + lang, config_.checks);
            }
        }
    }

    // Load output settings
    if (scanner_cfg.contains("output") && scanner_cfg["output"].is_object()) {
        auto& output = scanner_cfg["output"];
        if (output.contains("format")) config_.output_format = output["format"].get<std::string>();
        if (output.contains("max_issues_per_file")) config_.max_issues_per_file = output["max_issues_per_file"].get<int>();
        if (output.contains("max_total_issues")) config_.max_total_issues = output["max_total_issues"].get<int>();
        if (output.contains("group_by")) config_.group_by = output["group_by"].get<std::string>();
        if (output.contains("sort_by")) config_.sort_by = output["sort_by"].get<std::string>();
        if (output.contains("show_line_preview")) config_.show_line_preview = output["show_line_preview"].get<bool>();
        if (output.contains("show_fix_suggestion")) config_.show_fix_suggestion = output["show_fix_suggestion"].get<bool>();
        if (output.contains("save_json")) config_.save_json = output["save_json"].get<bool>();
        if (output.contains("save_text")) config_.save_text = output["save_text"].get<bool>();
        if (output.contains("save_sarif")) config_.save_sarif = output["save_sarif"].get<bool>();
        if (output.contains("json_path")) config_.json_path = output["json_path"].get<std::string>();
        if (output.contains("text_path")) config_.text_path = output["text_path"].get<std::string>();
        if (output.contains("sarif_path")) config_.sarif_path = output["sarif_path"].get<std::string>();
    }

    return true;
}

bool ScannerEngine::loadConfig(const std::string& target_path) {
    std::string path = discoverGovernJson(target_path);
    if (path.empty()) {
        fmt::print("Note: No govern.json found. Using default scanner config.\n");
        return false;
    }
    return loadConfigFromPath(path);
}

// ============================================================================
// File collection
// ============================================================================

std::vector<std::string> ScannerEngine::collectFiles(
    const std::string& target_path, const std::string& language) const {

    static const std::unordered_map<std::string, std::vector<std::string>> ext_map = {
        {"python", {".py"}},
        {"javascript", {".js", ".jsx", ".ts", ".tsx"}},
        {"cpp", {".cpp", ".cc", ".cxx", ".h", ".hpp"}},
        {"go", {".go"}},
        {"rust", {".rs"}},
        {"naab", {".naab"}},
    };

    std::unordered_set<std::string> valid_exts;
    if (language == "auto") {
        for (const auto& [_, exts] : ext_map) {
            for (const auto& e : exts) valid_exts.insert(e);
        }
    } else {
        auto it = ext_map.find(language);
        if (it != ext_map.end()) {
            for (const auto& e : it->second) valid_exts.insert(e);
        }
    }

    static const std::unordered_set<std::string> exclude_dirs = {
        "node_modules", ".git", "vendor", "build", "target", "dist", "__pycache__"
    };

    std::vector<std::string> found;
    std::error_code ec;

    if (fs::is_regular_file(target_path, ec)) {
        found.push_back(target_path);
        return found;
    }

    if (!fs::is_directory(target_path, ec)) {
        return found;
    }

    auto dir_opts = fs::directory_options::none;
    if (config_.follow_symlinks) {
        dir_opts = fs::directory_options::follow_directory_symlink;
    }

    auto it = fs::recursive_directory_iterator(target_path, dir_opts, ec);
    auto end_it = fs::recursive_directory_iterator();
    for (; it != end_it; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }

        if (it->is_directory(ec)) {
            std::string dirname = it->path().filename().string();
            if (exclude_dirs.count(dirname)) {
                it.disable_recursion_pending();
                continue;
            }
        }

        if (!it->is_regular_file(ec)) continue;

        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (valid_exts.count(ext)) {
            // Check file size
            auto fsize = it->file_size(ec);
            if (!ec && fsize <= static_cast<uintmax_t>(config_.max_file_size_kb * 1024)) {
                found.push_back(it->path().string());
            }
        }

        if (static_cast<int>(found.size()) >= config_.max_files) break;
    }

    std::sort(found.begin(), found.end());
    return found;
}

// ============================================================================
// Scanning
// ============================================================================

std::vector<Issue> ScannerEngine::scanFile(const std::string& filepath,
                                            const std::string& language) const {
    std::ifstream file(filepath);
    if (!file.is_open()) return {};

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    if (content.empty()) return {};

    auto lines = splitLines(content);
    std::vector<Issue> all_issues;

    // For NAAb files, strip polyglot blocks for most checks
    std::vector<std::string> check_lines = lines;
    if (language == "naab") {
        check_lines = stripPolyglotBlocks(lines);
    }

    // Run universal checks
    checkRedundancy(filepath, check_lines, language, all_issues);
    checkCodeQuality(filepath, check_lines, language, all_issues);
    checkComplexity(filepath, check_lines, language, all_issues);
    checkStyle(filepath, check_lines, content, language, all_issues);
    checkSecurity(filepath, check_lines, language, all_issues);

    // Run language-specific checks
    if (language == "python") {
        checkLangPython(filepath, lines, content, all_issues);
    } else if (language == "javascript") {
        checkLangJavascript(filepath, lines, all_issues);
    } else if (language == "cpp") {
        checkLangCpp(filepath, lines, all_issues);
    } else if (language == "go") {
        checkLangGo(filepath, lines, all_issues);
    } else if (language == "rust") {
        checkLangRust(filepath, lines, all_issues);
    } else if (language == "naab") {
        checkLangNaab(filepath, check_lines, lines, content, all_issues);
    }

    return all_issues;
}

ScanResult ScannerEngine::scan(const std::string& target_path,
                                const std::string& language) {
    ScanResult result;
    result.path = target_path;
    result.language = language;

    auto files = collectFiles(target_path, language);

    for (const auto& fpath : files) {
        std::string file_lang = language;
        if (file_lang == "auto") {
            file_lang = detectLanguage(fpath);
        }
        if (file_lang == "unknown") continue;

        auto issues = scanFile(fpath, file_lang);
        for (auto& issue : issues) {
            result.issues.push_back(std::move(issue));
        }
        result.files_scanned++;
    }

    return result;
}

// ============================================================================
// Report generation — Text
// ============================================================================

std::string ScannerEngine::formatTextReport(const ScanResult& result) const {
    int hard_count = 0, soft_count = 0, advisory_count = 0;
    for (const auto& issue : result.issues) {
        if (issue.level == "hard") hard_count++;
        else if (issue.level == "soft") soft_count++;
        else if (issue.level == "advisory") advisory_count++;
    }
    int total = static_cast<int>(result.issues.size());

    std::string out;
    out += "\n";
    out += "+" + std::string(50, '=') + "+\n";
    out += "|  CODE QUALITY REPORT - naab-q v1.0" + std::string(14, ' ') + "|\n";
    out += "+" + std::string(50, '=') + "+\n";
    out += "\n";
    out += fmt::format("Scanned: {} files | Language: {}\n", result.files_scanned, result.language);
    out += fmt::format("Issues: {} ({} hard, {} soft, {} advisory)\n", total, hard_count, soft_count, advisory_count);
    out += "\n";
    out += std::string(52, '-') + "\n";

    // Group by file
    std::map<std::string, std::vector<const Issue*>> files_map;
    for (const auto& issue : result.issues) {
        files_map[issue.file].push_back(&issue);
    }

    auto severity_order = [](const std::string& sev) -> int {
        if (sev == "critical") return 0;
        if (sev == "high") return 1;
        if (sev == "medium") return 2;
        if (sev == "low") return 3;
        return 4;
    };

    for (const auto& [fpath, file_issues] : files_map) {
        out += fmt::format("\n  {} ({} issues)\n\n", fpath, file_issues.size());

        // Group by rule+level
        struct RuleGroup {
            std::string key;
            std::vector<const Issue*> issues;
        };
        std::map<std::string, RuleGroup> rule_groups;
        for (const auto* iss : file_issues) {
            std::string key = iss->category + "." + iss->rule + "|" + iss->level;
            rule_groups[key].key = key;
            rule_groups[key].issues.push_back(iss);
        }

        // Sort by severity
        std::vector<std::pair<int, std::string>> sorted_keys;
        for (const auto& [key, group] : rule_groups) {
            sorted_keys.push_back({severity_order(group.issues[0]->severity), key});
        }
        std::sort(sorted_keys.begin(), sorted_keys.end());

        for (const auto& [_, key] : sorted_keys) {
            const auto& group = rule_groups[key];
            const auto* first = group.issues[0];
            std::string lvl_upper = first->level;
            std::transform(lvl_upper.begin(), lvl_upper.end(), lvl_upper.begin(), ::toupper);

            std::string icon;
            if (lvl_upper == "HARD") icon = "  X";
            else if (lvl_upper == "SOFT") icon = "  !";
            else icon = "  i";

            if (group.issues.size() == 1) {
                out += fmt::format("{} Line {} [{}] {}.{}\n", icon, first->line, lvl_upper, first->category, first->rule);
                out += fmt::format("     {}\n", first->message);
                if (!first->preview.empty()) {
                    out += fmt::format("     -> {}\n", first->preview.substr(0, 80));
                }
            } else {
                std::set<int> line_set;
                for (const auto* iss : group.issues) line_set.insert(iss->line);
                std::string line_nums;
                for (int ln : line_set) {
                    if (!line_nums.empty()) line_nums += ", ";
                    line_nums += std::to_string(ln);
                }
                out += fmt::format("{} [{}] {}.{} ({} occurrences)\n", icon, lvl_upper, first->category, first->rule, group.issues.size());
                out += fmt::format("     Lines: {}\n", line_nums);
                out += fmt::format("     {}\n", first->message);
                out += fmt::format("     -> {}\n", first->preview.substr(0, 80));
            }
            if (!first->fix.empty()) {
                out += fmt::format("     Fix: {}\n", first->fix);
            }
            out += "\n";
        }
    }

    out += std::string(52, '-') + "\n\n";

    // Summary by category
    std::map<std::string, std::array<int, 3>> cat_counts;  // [hard, soft, advisory]
    for (const auto& issue : result.issues) {
        if (issue.level == "hard") cat_counts[issue.category][0]++;
        else if (issue.level == "soft") cat_counts[issue.category][1]++;
        else cat_counts[issue.category][2]++;
    }

    out += "Summary by Category:\n";
    for (const auto& [cat, counts] : cat_counts) {
        int t = counts[0] + counts[1] + counts[2];
        std::string label = (t == 1) ? "issue" : "issues";
        out += fmt::format("  {:<20s} {:3d} {} ({} hard, {} soft, {} advisory)\n",
                          cat, t, label, counts[0], counts[1], counts[2]);
    }
    out += "\n";
    out += std::string(52, '=') + "\n";

    return out;
}

// ============================================================================
// Report generation — JSON
// ============================================================================

std::string ScannerEngine::formatJsonReport(const ScanResult& result) const {
    int hard_count = 0, soft_count = 0, advisory_count = 0;
    for (const auto& issue : result.issues) {
        if (issue.level == "hard") hard_count++;
        else if (issue.level == "soft") soft_count++;
        else if (issue.level == "advisory") advisory_count++;
    }

    json report;
    report["version"] = "1.0";
    report["scan_info"] = {
        {"path", result.path},
        {"language", result.language},
        {"files_scanned", result.files_scanned}
    };
    report["summary"] = {
        {"total_issues", static_cast<int>(result.issues.size())},
        {"by_level", {
            {"hard", hard_count},
            {"soft", soft_count},
            {"advisory", advisory_count}
        }}
    };

    json issues_arr = json::array();
    for (const auto& issue : result.issues) {
        issues_arr.push_back({
            {"file", issue.file},
            {"line", issue.line},
            {"rule", issue.rule},
            {"category", issue.category},
            {"level", issue.level},
            {"severity", issue.severity},
            {"message", issue.message},
            {"preview", issue.preview},
            {"fix", issue.fix}
        });
    }
    report["issues"] = issues_arr;

    return report.dump(2);
}

// ============================================================================
// Save reports to disk
// ============================================================================

void ScannerEngine::saveReports(const ScanResult& result) const {
    if (config_.save_text) {
        std::ofstream file(config_.text_path);
        if (file.is_open()) {
            file << formatTextReport(result);
            fmt::print("Saved text report: {}\n", config_.text_path);
        } else {
            fmt::print("Warning: Could not save text report: {}\n", config_.text_path);
        }
    }

    if (config_.save_json) {
        std::ofstream file(config_.json_path);
        if (file.is_open()) {
            file << formatJsonReport(result);
            fmt::print("Saved JSON report: {}\n", config_.json_path);
        } else {
            fmt::print("Warning: Could not save JSON report: {}\n", config_.json_path);
        }
    }
}

} // namespace scanner
} // namespace naab
