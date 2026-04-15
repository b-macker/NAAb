#pragma once

// NAAb Package Manager
// GitHub-based package management with governance integration

#include <string>
#include <vector>
#include <unordered_map>

namespace naab {
namespace packages {

struct PackageDep {
    std::string name;           // "http-utils"
    std::string github;         // "naab-community/http-utils"
    std::string version_spec;   // "^1.0.0", "1.2.0", "main"
    std::string path;           // Local path (for path deps)
    bool is_path_dep = false;
};

struct PackageLockEntry {
    std::string name;
    std::string version;
    std::string source;         // "github:user/repo"
    std::string commit;         // Git commit hash
    std::string integrity;      // "sha256:..."
};

struct PackageLock {
    std::vector<PackageLockEntry> packages;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
    bool hasPackage(const std::string& name) const;
    const PackageLockEntry* getPackage(const std::string& name) const;
};

struct PackageInfo {
    std::string name;
    std::string version;
    std::string description;
    std::string github;
    std::vector<std::string> keywords;
    std::string license;
    std::string main_file;      // "src/lib.naab"
    std::string plugin_file;    // "governance/checks.naab"
    std::string rules_file;     // "governance/rules.json"
    std::vector<PackageDep> dependencies;
    bool has_governance = false;
    bool has_claude_md = false;
};

class PackageManager {
public:
    explicit PackageManager(const std::string& project_dir);

    // Core operations
    bool install(const std::string& spec);          // "user/repo" or "user/repo@version"
    bool installAll();                               // Install all from naab.toml
    bool remove(const std::string& name);
    bool update(const std::string& name = "");      // empty = all
    std::vector<PackageInfo> list() const;
    PackageInfo info(const std::string& spec) const;
    std::vector<PackageInfo> search(const std::string& query) const;
    bool publish() const;                           // Validate and show instructions

    // Governance integration
    bool applyPackageGovernance(const std::string& package_name);
    bool removePackageGovernance(const std::string& package_name);

    // Error reporting
    const std::string& getLastError() const { return last_error_; }

private:
    std::string project_dir_;
    std::string modules_dir_;       // project_dir_/naab_modules/
    PackageLock lock_;
    std::string last_error_;
    std::string last_download_hash_;  // SHA-256 of last downloaded tarball

    // GitHub operations
    bool downloadFromGitHub(const std::string& owner, const std::string& repo,
                            const std::string& ref, const std::string& dest_dir);
    std::string getLatestRelease(const std::string& owner, const std::string& repo);
    std::vector<std::string> listTags(const std::string& owner, const std::string& repo);

    // HTTP helpers
    std::string httpGet(const std::string& url);
    bool httpDownloadFile(const std::string& url, const std::string& dest);

    // Spec parsing
    struct ParsedSpec {
        std::string owner;      // "naab-community"
        std::string repo;       // "http-utils"
        std::string version;    // "1.0.0", "main", "" (latest)
    };
    ParsedSpec parseSpec(const std::string& spec) const;

    // Dependency resolution
    bool resolveDependencies(const PackageInfo& pkg,
                             std::vector<PackageDep>& resolved,
                             std::vector<std::string>& installing);

    // Manifest operations
    bool addToManifest(const std::string& name, const std::string& github,
                       const std::string& version);
    bool removeFromManifest(const std::string& name);
    PackageInfo readPackageInfo(const std::string& package_dir) const;

    // Lockfile path
    std::string lockfilePath() const;
    std::string manifestPath() const;

    // Tarball handling
    bool extractTarball(const std::string& tarball_path, const std::string& dest_dir);
    std::string computeSHA256(const std::string& file_path);

    // Registry
    static const std::string REGISTRY_URL;
};

} // namespace packages
} // namespace naab
