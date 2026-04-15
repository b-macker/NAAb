// NAAb Package Manager Implementation
// GitHub-based package management with governance integration

#include "naab/package_manager.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <toml++/toml.h>
#include <fmt/core.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <openssl/sha.h>

namespace fs = std::filesystem;

namespace naab {
namespace packages {

const std::string PackageManager::REGISTRY_URL =
    "https://raw.githubusercontent.com/naab-community/registry/main/index.json";

// ============================================================================
// HTTP Helpers (using libcurl)
// ============================================================================

static size_t writeStringCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

static size_t writeFileCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* file = static_cast<std::ofstream*>(userp);
    file->write(static_cast<char*>(contents), static_cast<std::streamsize>(total));
    return total;
}

// Get auth token from environment
static std::string getGitHubToken() {
    const char* token = std::getenv("GITHUB_TOKEN");
    if (!token) token = std::getenv("GH_TOKEN");
    return token ? std::string(token) : "";
}

std::string PackageManager::httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        last_error_ = "Failed to initialize curl";
        return "";
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: NAAb-PackageManager/1.0");
    headers = curl_slist_append(headers, "Accept: application/vnd.github.v3+json");

    std::string token = getGitHubToken();
    if (!token.empty()) {
        headers = curl_slist_append(headers,
            ("Authorization: Bearer " + token).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        last_error_ = fmt::format("HTTP request failed: {}", curl_easy_strerror(res));
        return "";
    }

    if (http_code == 403) {
        last_error_ = "GitHub API rate limit exceeded (60 requests/hour for unauthenticated users)\n\n"
            "  Help:\n"
            "  - Set GITHUB_TOKEN or GH_TOKEN environment variable for 5000 requests/hour\n"
            "  - Run: export GITHUB_TOKEN=$(gh auth token)\n";
        return "";
    }

    if (http_code < 200 || http_code >= 300) {
        last_error_ = fmt::format("HTTP {} from {}", http_code, url);
        return "";
    }

    return response;
}

bool PackageManager::httpDownloadFile(const std::string& url, const std::string& dest) {
    fs::create_directories(fs::path(dest).parent_path());

    std::ofstream file(dest, std::ios::binary);
    if (!file.is_open()) {
        last_error_ = fmt::format("Cannot create file: {}", dest);
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) { file.close(); return false; }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: NAAb-PackageManager/1.0");

    std::string token = getGitHubToken();
    if (!token.empty()) {
        headers = curl_slist_append(headers,
            ("Authorization: Bearer " + token).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    file.close();

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        fs::remove(dest);
        last_error_ = fmt::format("Download failed: HTTP {}", http_code);
        return false;
    }

    return true;
}

// ============================================================================
// GitHub API
// ============================================================================

std::string PackageManager::getLatestRelease(const std::string& owner, const std::string& repo) {
    std::string url = fmt::format("https://api.github.com/repos/{}/{}/releases/latest", owner, repo);
    std::string response = httpGet(url);
    if (response.empty()) {
        // No releases — try default branch
        return "main";
    }

    try {
        auto j = nlohmann::json::parse(response);
        if (j.contains("tag_name")) {
            std::string tag = j["tag_name"].get<std::string>();
            // Strip leading 'v' if present
            if (!tag.empty() && tag[0] == 'v') tag = tag.substr(1);
            return tag;
        }
    } catch (...) {}

    return "main";
}

std::vector<std::string> PackageManager::listTags(const std::string& owner, const std::string& repo) {
    std::string url = fmt::format("https://api.github.com/repos/{}/{}/tags?per_page=100", owner, repo);
    std::string response = httpGet(url);
    std::vector<std::string> tags;
    if (response.empty()) return tags;

    try {
        auto j = nlohmann::json::parse(response);
        for (auto& tag : j) {
            if (tag.contains("name")) {
                tags.push_back(tag["name"].get<std::string>());
            }
        }
    } catch (...) {}

    return tags;
}

bool PackageManager::downloadFromGitHub(const std::string& owner, const std::string& repo,
                                         const std::string& ref, const std::string& dest_dir) {
    // Download tarball from GitHub
    std::string tarball_url = fmt::format(
        "https://api.github.com/repos/{}/{}/tarball/{}", owner, repo, ref);

    std::string cache_dir;
    const char* home = std::getenv("HOME");
    if (home) {
        cache_dir = std::string(home) + "/.naab/cache";
    } else {
        cache_dir = "/tmp/naab-cache";
    }
    fs::create_directories(cache_dir);

    std::string tarball_path = cache_dir + "/" + owner + "-" + repo + "-" + ref + ".tar.gz";

    fmt::print("  Downloading {}/{}@{} ...\n", owner, repo, ref);

    if (!httpDownloadFile(tarball_url, tarball_path)) {
        return false;
    }

    // Check file size
    auto file_size = fs::file_size(tarball_path);
    if (file_size > 50 * 1024 * 1024) {
        fs::remove(tarball_path);
        last_error_ = fmt::format("Package tarball is too large ({} MB, limit 50 MB)",
                                   file_size / (1024 * 1024));
        return false;
    }
    if (file_size > 10 * 1024 * 1024) {
        fmt::print("  Warning: Package tarball is {} MB (large for a NAAb package)\n",
                   file_size / (1024 * 1024));
    }

    // Extract tarball
    if (!extractTarball(tarball_path, dest_dir)) {
        fs::remove(tarball_path);
        return false;
    }

    // Clean up tarball
    fs::remove(tarball_path);
    return true;
}

bool PackageManager::extractTarball(const std::string& tarball_path, const std::string& dest_dir) {
    // Create temp extraction directory
    std::string temp_dir = dest_dir + ".extracting";
    fs::create_directories(temp_dir);

    // Extract using tar command
    std::string cmd = fmt::format("tar xzf '{}' -C '{}' 2>/dev/null", tarball_path, temp_dir);
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        fs::remove_all(temp_dir);
        last_error_ = "Failed to extract tarball (is 'tar' installed?)";
        return false;
    }

    // GitHub tarballs extract to owner-repo-hash/ — find the single subdirectory
    std::string extracted_subdir;
    for (auto& entry : fs::directory_iterator(temp_dir)) {
        if (entry.is_directory()) {
            extracted_subdir = entry.path().string();
            break;
        }
    }

    if (extracted_subdir.empty()) {
        fs::remove_all(temp_dir);
        last_error_ = "Tarball extraction produced no directory";
        return false;
    }

    // Remove existing destination and move extracted content
    if (fs::exists(dest_dir)) {
        fs::remove_all(dest_dir);
    }
    fs::rename(extracted_subdir, dest_dir);
    fs::remove_all(temp_dir);

    return true;
}

std::string PackageManager::computeSHA256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) return "";

    SHA256_CTX sha256;
    SHA256_Init(&sha256);

    char buffer[8192];
    while (file.read(buffer, sizeof(buffer))) {
        SHA256_Update(&sha256, buffer, file.gcount());
    }
    if (file.gcount() > 0) {
        SHA256_Update(&sha256, buffer, file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);

    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", hash[i]);
        hex += buf;
    }

    return "sha256:" + hex;
}

// ============================================================================
// Spec Parsing
// ============================================================================

PackageManager::ParsedSpec PackageManager::parseSpec(const std::string& spec) const {
    ParsedSpec result;

    // Parse "owner/repo@version" or "owner/repo"
    std::string s = spec;

    // Split on @
    auto at_pos = s.find('@');
    if (at_pos != std::string::npos) {
        result.version = s.substr(at_pos + 1);
        s = s.substr(0, at_pos);
        // Strip leading 'v' from version
        if (!result.version.empty() && result.version[0] == 'v') {
            result.version = result.version.substr(1);
        }
    }

    // Split on /
    auto slash_pos = s.find('/');
    if (slash_pos != std::string::npos) {
        result.owner = s.substr(0, slash_pos);
        result.repo = s.substr(slash_pos + 1);
    } else {
        // No owner specified — assume naab-community
        result.owner = "naab-community";
        result.repo = s;
    }

    return result;
}

// ============================================================================
// Package Info Reading
// ============================================================================

PackageInfo PackageManager::readPackageInfo(const std::string& package_dir) const {
    PackageInfo info;
    fs::path manifest_path = fs::path(package_dir) / "naab.toml";

    if (fs::exists(manifest_path)) {
        try {
            auto config = toml::parse_file(manifest_path.string());

            if (auto pkg = config["package"]) {
                info.name = pkg["name"].value_or("");
                info.version = pkg["version"].value_or("0.0.0");
                info.description = pkg["description"].value_or("");
                info.license = pkg["license"].value_or("");
                info.github = pkg["repository"].value_or("");

                if (auto kw = pkg["keywords"].as_array()) {
                    for (auto& k : *kw) {
                        info.keywords.push_back(k.value_or(""));
                    }
                }
            }

            if (auto gov = config["package"]["governance"]) {
                info.plugin_file = gov["plugin_file"].value_or("");
                info.rules_file = gov["rules_file"].value_or("");
                if (!info.plugin_file.empty() || !info.rules_file.empty()) {
                    info.has_governance = true;
                }
            }

            if (auto exports = config["exports"]) {
                info.main_file = exports["main"].value_or("src/lib.naab");
            } else {
                info.main_file = "src/lib.naab";
            }

            if (auto deps = config["dependencies"].as_table()) {
                for (auto& [key, val] : *deps) {
                    PackageDep dep;
                    dep.name = std::string(key);
                    if (val.is_table()) {
                        auto& t = *val.as_table();
                        dep.github = t["github"].value_or("");
                        dep.version_spec = t["version"].value_or("");
                        dep.path = t["path"].value_or("");
                        dep.is_path_dep = !dep.path.empty();
                    } else if (val.is_string()) {
                        dep.version_spec = val.value_or("");
                    }
                    info.dependencies.push_back(std::move(dep));
                }
            }

        } catch (const toml::parse_error& e) {
            fmt::print(stderr, "[package] Warning: Failed to parse {}: {}\n",
                       manifest_path.string(), e.what());
        }
    } else {
        // No naab.toml — infer from directory
        info.name = fs::path(package_dir).filename().string();
        info.version = "0.0.0";

        // Look for entry point
        for (auto& candidate : {"src/lib.naab", "lib.naab", "index.naab", "main.naab"}) {
            if (fs::exists(fs::path(package_dir) / candidate)) {
                info.main_file = candidate;
                break;
            }
        }
    }

    // Check for CLAUDE.md
    info.has_claude_md = fs::exists(fs::path(package_dir) / "CLAUDE.md");

    // Check for governance directory
    if (!info.has_governance) {
        fs::path gov_dir = fs::path(package_dir) / "governance";
        if (fs::exists(gov_dir) && fs::is_directory(gov_dir)) {
            // Auto-detect governance files
            if (fs::exists(gov_dir / "rules.json")) {
                info.rules_file = "governance/rules.json";
                info.has_governance = true;
            }
            // Find .naab plugin files
            for (auto& entry : fs::directory_iterator(gov_dir)) {
                if (entry.path().extension() == ".naab") {
                    info.plugin_file = "governance/" + entry.path().filename().string();
                    info.has_governance = true;
                    break;
                }
            }
        }
    }

    return info;
}

// ============================================================================
// Constructor
// ============================================================================

PackageManager::PackageManager(const std::string& project_dir)
    : project_dir_(fs::absolute(project_dir).string()),
      modules_dir_(project_dir_ + "/naab_modules") {
    // Load lockfile if it exists
    lock_.load(lockfilePath());
}

std::string PackageManager::lockfilePath() const {
    return project_dir_ + "/naab.lock";
}

std::string PackageManager::manifestPath() const {
    return project_dir_ + "/naab.toml";
}

// ============================================================================
// Core: Install
// ============================================================================

bool PackageManager::install(const std::string& spec) {
    auto parsed = parseSpec(spec);

    if (parsed.repo.empty()) {
        last_error_ = "Invalid package spec: " + spec + "\n\n"
            "  Usage: naab install user/package\n"
            "         naab install user/package@1.0.0\n"
            "         naab install package-name       (from naab-community)\n";
        return false;
    }

    // Resolve version
    std::string version = parsed.version;
    if (version.empty()) {
        version = getLatestRelease(parsed.owner, parsed.repo);
        if (version.empty()) {
            last_error_ = fmt::format("Cannot find releases for {}/{}", parsed.owner, parsed.repo);
            return false;
        }
    }

    // Determine package name (repo name)
    std::string pkg_name = parsed.repo;
    std::string pkg_dir = modules_dir_ + "/" + pkg_name;

    // Check if already installed at this version
    if (fs::exists(pkg_dir)) {
        auto existing = readPackageInfo(pkg_dir);
        if (existing.version == version) {
            fmt::print("  {} {} already installed at version {}\n", pkg_name, version, version);
            return true;
        }
        fmt::print("  Updating {} {} → {} ...\n", pkg_name, existing.version, version);
    }

    // Create naab_modules directory
    fs::create_directories(modules_dir_);

    // Try version tag with and without 'v' prefix
    std::string ref = version;
    if (!downloadFromGitHub(parsed.owner, parsed.repo, ref, pkg_dir)) {
        // Try with 'v' prefix
        ref = "v" + version;
        if (!downloadFromGitHub(parsed.owner, parsed.repo, ref, pkg_dir)) {
            return false;
        }
    }

    // Read package info
    auto pkg_info = readPackageInfo(pkg_dir);
    if (pkg_info.name.empty()) pkg_info.name = pkg_name;
    if (pkg_info.version.empty() || pkg_info.version == "0.0.0") pkg_info.version = version;

    fmt::print("  ✓ Installed {}@{} → naab_modules/{}/\n", pkg_name, pkg_info.version, pkg_name);

    // Resolve transitive dependencies
    std::vector<std::string> installing = {pkg_name};
    for (const auto& dep : pkg_info.dependencies) {
        if (dep.is_path_dep) continue;
        if (dep.github.empty()) continue;

        // Check if already installed
        if (fs::exists(modules_dir_ + "/" + dep.name)) {
            continue;
        }

        // Recursive install
        std::string dep_spec = dep.github;
        if (!dep.version_spec.empty()) {
            // Strip ^ or ~ for now — use exact version
            std::string v = dep.version_spec;
            if (!v.empty() && (v[0] == '^' || v[0] == '~')) v = v.substr(1);
            dep_spec += "@" + v;
        }
        if (install(dep_spec)) {
            fmt::print("    (dependency of {})\n", pkg_name);
        }
    }

    // Apply governance if package has it
    if (pkg_info.has_governance) {
        applyPackageGovernance(pkg_name);
    }

    // Update naab.toml
    addToManifest(pkg_name, parsed.owner + "/" + parsed.repo, pkg_info.version);

    // Update lockfile
    PackageLockEntry entry;
    entry.name = pkg_name;
    entry.version = pkg_info.version;
    entry.source = "github:" + parsed.owner + "/" + parsed.repo;
    entry.commit = ref;

    // Remove existing entry for this package
    lock_.packages.erase(
        std::remove_if(lock_.packages.begin(), lock_.packages.end(),
            [&](const PackageLockEntry& e) { return e.name == pkg_name; }),
        lock_.packages.end());
    lock_.packages.push_back(entry);
    lock_.save(lockfilePath());

    return true;
}

bool PackageManager::installAll() {
    // Read naab.toml dependencies
    std::string manifest = manifestPath();
    if (!fs::exists(manifest)) {
        last_error_ = "No naab.toml found in current directory";
        return false;
    }

    try {
        auto config = toml::parse_file(manifest);
        auto deps = config["dependencies"].as_table();
        if (!deps) {
            fmt::print("No dependencies in naab.toml\n");
            return true;
        }

        int installed = 0;
        int skipped = 0;

        for (auto& [key, val] : *deps) {
            std::string name = std::string(key);
            std::string github;
            std::string version;

            if (val.is_string()) {
                // Simple form: "user/repo" = ">= 0.5.0" or "user/repo" = "1.0.0"
                // Treat the key as the package spec (owner/repo)
                version = val.as_string()->get();
                // Strip version operators
                while (!version.empty() && (version[0] == '>' || version[0] == '=' ||
                       version[0] == '<' || version[0] == '^' || version[0] == '~' ||
                       version[0] == ' ')) {
                    version = version.substr(1);
                }
                // name is already "user/repo"
                github = name;
            } else if (val.is_table()) {
                auto& t = *val.as_table();
                github = t["github"].value_or("");
                version = t["version"].value_or("");
                if (!version.empty() && (version[0] == '^' || version[0] == '~'))
                    version = version.substr(1);

                // Handle git URL dependencies
                std::string git_url = t["git"].value_or("");
                if (!git_url.empty() && github.empty()) {
                    std::string ref = t["ref"].value_or("");
                    std::string tag = t["tag"].value_or("");
                    std::string commit = t["commit"].value_or("");
                    if (ref.empty() && tag.empty() && commit.empty()) {
                        fmt::print(stderr, "  ⚠ Security warning: dependency '{}' uses an unpinned git URL\n"
                            "    URL: {}\n"
                            "    Fix: Add a 'commit', 'tag', or 'ref' field to pin the version:\n"
                            "      \"{}\" = {{ git = \"{}\", commit = \"abc123...\" }}\n",
                            name, git_url, name, git_url);
                    }
                    fmt::print(stderr, "  Skipped '{}': git URL dependencies not yet supported\n", name);
                    skipped++;
                    continue;
                }
            }

            if (github.empty()) {
                fmt::print(stderr, "  Skipped '{}': no github source specified\n", name);
                skipped++;
                continue;
            }

            std::string spec = github;
            if (!version.empty()) spec += "@" + version;

            if (!install(spec)) {
                fmt::print(stderr, "  Failed to install '{}': {}\n", name, last_error_);
                skipped++;
            } else {
                installed++;
            }
        }

        fmt::print("Install complete: {} installed, {} skipped\n", installed, skipped);
        fflush(stdout);
        return true;
    } catch (const std::exception& e) {
        last_error_ = fmt::format("Failed to parse naab.toml: {}", e.what());
        return false;
    }
}

// ============================================================================
// Core: Remove
// ============================================================================

bool PackageManager::remove(const std::string& name) {
    std::string pkg_dir = modules_dir_ + "/" + name;

    if (!fs::exists(pkg_dir)) {
        last_error_ = fmt::format("Package '{}' is not installed", name);
        return false;
    }

    // Remove governance entries first
    removePackageGovernance(name);

    // Remove from filesystem
    fs::remove_all(pkg_dir);

    // Remove from naab.toml
    removeFromManifest(name);

    // Remove from lockfile
    lock_.packages.erase(
        std::remove_if(lock_.packages.begin(), lock_.packages.end(),
            [&](const PackageLockEntry& e) { return e.name == name; }),
        lock_.packages.end());
    lock_.save(lockfilePath());

    fmt::print("  ✓ Removed {}\n", name);
    return true;
}

// ============================================================================
// Core: List
// ============================================================================

std::vector<PackageInfo> PackageManager::list() const {
    std::vector<PackageInfo> result;

    if (!fs::exists(modules_dir_)) return result;

    for (auto& entry : fs::directory_iterator(modules_dir_)) {
        if (!entry.is_directory()) continue;
        auto info = readPackageInfo(entry.path().string());
        if (info.name.empty()) info.name = entry.path().filename().string();
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(),
        [](const PackageInfo& a, const PackageInfo& b) { return a.name < b.name; });

    return result;
}

// ============================================================================
// Core: Info
// ============================================================================

PackageInfo PackageManager::info(const std::string& spec) const {
    auto parsed = parseSpec(spec);
    std::string pkg_dir = modules_dir_ + "/" + parsed.repo;

    // If installed locally, read from there
    if (fs::exists(pkg_dir)) {
        return readPackageInfo(pkg_dir);
    }

    // Otherwise, return minimal info from spec
    PackageInfo info;
    info.name = parsed.repo;
    info.github = parsed.owner + "/" + parsed.repo;
    return info;
}

// ============================================================================
// Core: Search
// ============================================================================

std::vector<PackageInfo> PackageManager::search(const std::string& query) const {
    std::vector<PackageInfo> results;

    // Fetch registry index
    // Use const_cast because httpGet modifies last_error_ which is fine
    auto* self = const_cast<PackageManager*>(this);
    std::string response = self->httpGet(REGISTRY_URL);
    if (response.empty()) return results;

    try {
        auto j = nlohmann::json::parse(response);
        if (!j.contains("packages")) return results;

        std::string q = query;
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);

        for (auto& [name, pkg] : j["packages"].items()) {
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

            std::string desc = pkg.value("description", "");
            std::string desc_lower = desc;
            std::transform(desc_lower.begin(), desc_lower.end(), desc_lower.begin(), ::tolower);

            bool matches = name_lower.find(q) != std::string::npos ||
                           desc_lower.find(q) != std::string::npos;

            if (!matches && pkg.contains("keywords")) {
                for (auto& kw : pkg["keywords"]) {
                    std::string kw_str = kw.get<std::string>();
                    std::transform(kw_str.begin(), kw_str.end(), kw_str.begin(), ::tolower);
                    if (kw_str.find(q) != std::string::npos) {
                        matches = true;
                        break;
                    }
                }
            }

            if (matches) {
                PackageInfo info;
                info.name = name;
                info.description = desc;
                info.github = pkg.value("github", "");
                info.version = pkg.value("latest", "");
                if (pkg.contains("keywords")) {
                    for (auto& kw : pkg["keywords"]) {
                        info.keywords.push_back(kw.get<std::string>());
                    }
                }
                results.push_back(std::move(info));
            }
        }
    } catch (...) {}

    return results;
}

// ============================================================================
// Core: Update
// ============================================================================

bool PackageManager::update(const std::string& name) {
    if (!name.empty()) {
        // Update specific package
        std::string pkg_dir = modules_dir_ + "/" + name;
        if (!fs::exists(pkg_dir)) {
            last_error_ = fmt::format("Package '{}' is not installed", name);
            return false;
        }

        auto info = readPackageInfo(pkg_dir);
        if (info.github.empty()) {
            // Try to find in lockfile
            auto* entry = lock_.getPackage(name);
            if (entry && entry->source.substr(0, 7) == "github:") {
                info.github = entry->source.substr(7);
            }
        }

        if (info.github.empty()) {
            last_error_ = fmt::format("Cannot determine source for '{}'", name);
            return false;
        }

        return install(info.github);
    }

    // Update all packages
    auto packages = list();
    bool all_ok = true;
    for (const auto& pkg : packages) {
        if (pkg.github.empty()) continue;
        fmt::print("Updating {} ...\n", pkg.name);
        if (!install(pkg.github)) {
            fmt::print(stderr, "  Warning: Failed to update {}: {}\n", pkg.name, last_error_);
            all_ok = false;
        }
    }
    return all_ok;
}

// ============================================================================
// Core: Publish
// ============================================================================

bool PackageManager::publish() const {
    std::string manifest = manifestPath();
    if (!fs::exists(manifest)) {
        fmt::print(stderr, "Error: No naab.toml found\n");
        return false;
    }

    auto info = readPackageInfo(project_dir_);
    bool valid = true;

    auto check = [&](bool ok, const std::string& msg) {
        fmt::print("  {} {}\n", ok ? "✓" : "✗", msg);
        if (!ok) valid = false;
    };

    fmt::print("\nPackage validation:\n");
    check(!info.name.empty(), "Package name: " + info.name);
    check(!info.version.empty() && info.version != "0.0.0", "Version: " + info.version);
    check(!info.main_file.empty() &&
          fs::exists(fs::path(project_dir_) / info.main_file),
          "Entry point exists: " + info.main_file);
    check(info.has_claude_md, "CLAUDE.md exists (recommended for AI users)");

    if (info.has_governance) {
        bool gov_ok = true;
        if (!info.plugin_file.empty()) {
            gov_ok = fs::exists(fs::path(project_dir_) / info.plugin_file);
        }
        check(gov_ok, "Governance plugin validates");
    }

    if (!valid) {
        fmt::print("\n  Fix the issues above before publishing.\n\n");
        return false;
    }

    fmt::print("\nTo publish:\n");
    fmt::print("  1. Push to GitHub: git push origin main\n");
    fmt::print("  2. Create a release: gh release create v{}\n", info.version);
    fmt::print("  3. Submit to registry: open a PR to naab-community/registry\n");
    fmt::print("     adding your package to index.json\n\n");

    return true;
}

// ============================================================================
// Governance Integration
// ============================================================================

bool PackageManager::applyPackageGovernance(const std::string& package_name) {
    std::string pkg_dir = modules_dir_ + "/" + package_name;
    auto info = readPackageInfo(pkg_dir);

    if (!info.has_governance) return true;  // Nothing to apply

    // Read rules.json from package
    std::string rules_path = pkg_dir + "/" + info.rules_file;
    if (!fs::exists(rules_path)) {
        // No rules file — governance plugin will still be loaded by the engine
        // if referenced in govern.json, but we won't auto-add it
        return true;
    }

    // Read project's govern.json
    std::string govern_path = project_dir_ + "/govern.json";
    nlohmann::json govern;

    if (fs::exists(govern_path)) {
        try {
            std::ifstream ifs(govern_path);
            govern = nlohmann::json::parse(ifs);
        } catch (...) {
            fmt::print(stderr, "  Warning: Could not parse govern.json, skipping governance integration\n");
            return false;
        }
    } else {
        // Create minimal govern.json
        govern = {{"version", "4.0"}, {"mode", "enforce"}};
    }

    // Read package rules
    nlohmann::json rules;
    try {
        std::ifstream ifs(rules_path);
        rules = nlohmann::json::parse(ifs);
    } catch (...) {
        fmt::print(stderr, "  Warning: Could not parse {}, skipping governance\n", rules_path);
        return false;
    }

    // Ensure governance_plugins array exists
    if (!govern.contains("governance_plugins") || !govern["governance_plugins"].is_array()) {
        govern["governance_plugins"] = nlohmann::json::array();
    }

    // Check if package governance already exists
    for (auto& plugin : govern["governance_plugins"]) {
        if (plugin.contains("_package") && plugin["_package"] == package_name) {
            // Already registered — update it
            plugin["file"] = "naab_modules/" + package_name + "/" + info.plugin_file;
            plugin["rules"] = rules.contains("rules") ? rules["rules"] : nlohmann::json::array();
            plugin["_managed_by"] = "naab-package-manager";
            plugin["_package"] = package_name;

            // Prefix rule IDs with package name
            if (plugin.contains("rules") && plugin["rules"].is_array()) {
                for (auto& rule : plugin["rules"]) {
                    if (rule.contains("id")) {
                        std::string id = rule["id"].get<std::string>();
                        if (id.find(package_name + ".") != 0) {
                            rule["id"] = package_name + "." + id;
                        }
                    }
                }
            }

            std::ofstream ofs(govern_path);
            ofs << govern.dump(2) << std::endl;
            return true;
        }
    }

    // Add new plugin entry
    nlohmann::json plugin_entry;
    plugin_entry["file"] = "naab_modules/" + package_name + "/" + info.plugin_file;
    plugin_entry["rules"] = rules.contains("rules") ? rules["rules"] : nlohmann::json::array();
    plugin_entry["_managed_by"] = "naab-package-manager";
    plugin_entry["_package"] = package_name;

    // Prefix rule IDs
    if (plugin_entry.contains("rules") && plugin_entry["rules"].is_array()) {
        for (auto& rule : plugin_entry["rules"]) {
            if (rule.contains("id")) {
                std::string id = rule["id"].get<std::string>();
                if (id.find(package_name + ".") != 0) {
                    rule["id"] = package_name + "." + id;
                }
            }
        }
    }

    govern["governance_plugins"].push_back(plugin_entry);

    // Write updated govern.json
    std::ofstream ofs(govern_path);
    ofs << govern.dump(2) << std::endl;

    int rule_count = plugin_entry["rules"].size();
    fmt::print("  ✓ Applied {} governance rule{} from {}\n",
               rule_count, rule_count == 1 ? "" : "s", package_name);

    // Print rule summaries
    for (auto& rule : plugin_entry["rules"]) {
        std::string id = rule.value("id", "");
        std::string desc = rule.value("description", "");
        std::string level = rule.value("level", "advisory");
        fmt::print("    - {}: {} ({})\n", id, desc, level);
    }

    return true;
}

bool PackageManager::removePackageGovernance(const std::string& package_name) {
    std::string govern_path = project_dir_ + "/govern.json";
    if (!fs::exists(govern_path)) return true;

    try {
        std::ifstream ifs(govern_path);
        auto govern = nlohmann::json::parse(ifs);
        ifs.close();

        if (!govern.contains("governance_plugins") || !govern["governance_plugins"].is_array()) {
            return true;
        }

        // Remove entries managed by package manager for this package
        auto& plugins = govern["governance_plugins"];
        nlohmann::json filtered = nlohmann::json::array();
        for (auto& plugin : plugins) {
            if (plugin.contains("_package") && plugin["_package"] == package_name &&
                plugin.contains("_managed_by") && plugin["_managed_by"] == "naab-package-manager") {
                continue;  // Skip — remove this entry
            }
            filtered.push_back(plugin);
        }
        govern["governance_plugins"] = filtered;

        std::ofstream ofs(govern_path);
        ofs << govern.dump(2) << std::endl;

    } catch (...) {
        // Non-fatal
    }

    return true;
}

// ============================================================================
// Manifest Operations
// ============================================================================

bool PackageManager::addToManifest(const std::string& name, const std::string& github,
                                    const std::string& version) {
    std::string manifest = manifestPath();
    if (!fs::exists(manifest)) return true;  // No manifest to update

    try {
        auto config = toml::parse_file(manifest);

        // Check if dependency already exists
        if (auto deps = config["dependencies"].as_table()) {
            if (deps->contains(name)) {
                return true;  // Already in manifest
            }
        }

        // Append dependency to file (toml++ doesn't support round-trip editing well)
        // So we'll append to the file directly
        std::ifstream ifs(manifest);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        ifs.close();

        // Find [dependencies] section or add one
        auto deps_pos = content.find("[dependencies]");
        if (deps_pos != std::string::npos) {
            // Find end of dependencies section (next section or EOF)
            auto next_section = content.find("\n[", deps_pos + 1);
            std::string insert_pos_str;
            size_t insert_pos;
            if (next_section != std::string::npos) {
                insert_pos = next_section;
            } else {
                insert_pos = content.size();
            }

            std::string dep_line = fmt::format(
                "{} = {{ github = \"{}\", version = \"{}\" }}\n",
                name, github, version);

            content.insert(insert_pos, dep_line);
        } else {
            // Add dependencies section
            content += fmt::format(
                "\n[dependencies]\n"
                "{} = {{ github = \"{}\", version = \"{}\" }}\n",
                name, github, version);
        }

        std::ofstream ofs(manifest);
        ofs << content;

    } catch (...) {
        // Non-fatal
    }

    return true;
}

bool PackageManager::removeFromManifest(const std::string& name) {
    std::string manifest = manifestPath();
    if (!fs::exists(manifest)) return true;

    try {
        std::ifstream ifs(manifest);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        ifs.close();

        // Remove lines that start with the package name in [dependencies]
        std::istringstream stream(content);
        std::string result;
        std::string line;
        while (std::getline(stream, line)) {
            // Skip lines that start with the package name (possibly with whitespace)
            std::string trimmed = line;
            while (!trimmed.empty() && (trimmed[0] == ' ' || trimmed[0] == '\t'))
                trimmed = trimmed.substr(1);
            if (trimmed.find(name + " ") == 0 || trimmed.find(name + "=") == 0) {
                continue;  // Skip this line
            }
            result += line + "\n";
        }

        std::ofstream ofs(manifest);
        ofs << result;

    } catch (...) {
        // Non-fatal
    }

    return true;
}

// ============================================================================
// Lockfile
// ============================================================================

bool PackageLock::load(const std::string& path) {
    if (!fs::exists(path)) return false;

    try {
        auto config = toml::parse_file(path);

        if (auto pkgs = config["package"].as_array()) {
            for (auto& node : *pkgs) {
                auto* pkg = node.as_table();
                if (!pkg) continue;
                PackageLockEntry entry;
                entry.name = (*pkg)["name"].value_or("");
                entry.version = (*pkg)["version"].value_or("");
                entry.source = (*pkg)["source"].value_or("");
                entry.commit = (*pkg)["commit"].value_or("");
                entry.integrity = (*pkg)["integrity"].value_or("");
                packages.push_back(std::move(entry));
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool PackageLock::save(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "# Auto-generated by naab install. Do not edit.\n";
    file << "# This file ensures reproducible builds.\n\n";

    for (const auto& pkg : packages) {
        file << "[[package]]\n";
        file << "name = \"" << pkg.name << "\"\n";
        file << "version = \"" << pkg.version << "\"\n";
        file << "source = \"" << pkg.source << "\"\n";
        file << "commit = \"" << pkg.commit << "\"\n";
        if (!pkg.integrity.empty()) {
            file << "integrity = \"" << pkg.integrity << "\"\n";
        }
        file << "\n";
    }

    return true;
}

bool PackageLock::hasPackage(const std::string& name) const {
    for (const auto& pkg : packages) {
        if (pkg.name == name) return true;
    }
    return false;
}

const PackageLockEntry* PackageLock::getPackage(const std::string& name) const {
    for (const auto& pkg : packages) {
        if (pkg.name == name) return &pkg;
    }
    return nullptr;
}

} // namespace packages
} // namespace naab
