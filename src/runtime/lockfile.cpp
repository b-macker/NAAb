//
// NAAb Lockfile — naab.lock implementation
// Records and verifies observed polyglot runtime versions.
//

#include "naab/lockfile.h"
#include "naab/config.h"
#include "naab/crypto_utils.h"
#include "naab/secure_file.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <chrono>
#include <ctime>
#include <cstdlib>

namespace naab {

// ============================================================================
// Helpers
// ============================================================================

static std::string currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buf;
}

static std::string detectPlatform() {
    std::string os = "linux";
    std::string arch = "unknown";
#if defined(__APPLE__)
    os = "darwin";
#elif defined(_WIN32)
    os = "windows";
#endif
#if defined(__aarch64__)
    arch = "arm64";
#elif defined(__x86_64__)
    arch = "x86_64";
#elif defined(__arm__)
    arch = "arm";
#endif
    return os + "/" + arch;
}

// ============================================================================
// Lockfile::load
// ============================================================================

Lockfile Lockfile::load(const std::string& path) {
    Lockfile lf;
    lf.naab_version = NAAB_VERSION_STRING;
    lf.platform = detectPlatform();

    std::ifstream ifs(path);
    if (!ifs.is_open()) return lf;

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (j.contains("naab_version") && j["naab_version"].is_string())
            lf.naab_version = j["naab_version"].get<std::string>();
        if (j.contains("platform") && j["platform"].is_string())
            lf.platform = j["platform"].get<std::string>();
        if (j.contains("runtimes") && j["runtimes"].is_array()) {
            for (const auto& entry : j["runtimes"]) {
                LockfileEntry e;
                if (entry.contains("language") && entry["language"].is_string())
                    e.language = entry["language"].get<std::string>();
                if (entry.contains("runtime_version") && entry["runtime_version"].is_string())
                    e.runtime_version = entry["runtime_version"].get<std::string>();
                if (entry.contains("binary_path") && entry["binary_path"].is_string())
                    e.binary_path = entry["binary_path"].get<std::string>();
                if (entry.contains("timestamp") && entry["timestamp"].is_string())
                    e.timestamp = entry["timestamp"].get<std::string>();
                if (!e.language.empty())
                    lf.runtimes.push_back(e);
            }
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[lockfile] Warning: failed to parse %s: %s\n",
                path.c_str(), e.what());
        lf.parse_failed = true;
    } catch (...) {
        fprintf(stderr, "[lockfile] Warning: failed to parse %s\n", path.c_str());
        lf.parse_failed = true;
    }
    return lf;
}

// ============================================================================
// Lockfile::save
// ============================================================================

void Lockfile::save(const std::string& path) const {
    // Don't overwrite a valid lockfile with data from a failed parse
    if (parse_failed) {
        fprintf(stderr, "[lockfile] Skipping save — parse failed, preserving existing %s\n",
                path.c_str());
        return;
    }
    // Create parent directory if needed
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    nlohmann::json j;
    j["naab_version"] = naab_version;
    j["platform"] = platform;
    j["runtimes"] = nlohmann::json::array();
    for (const auto& e : runtimes) {
        nlohmann::json entry;
        entry["language"] = e.language;
        entry["runtime_version"] = e.runtime_version;
        entry["binary_path"] = e.binary_path;
        entry["timestamp"] = e.timestamp;
        j["runtimes"].push_back(entry);
    }

    std::string content = j.dump(2) + "\n";

    // V-SC-005 (R24): refuse to follow symlinks when writing the lockfile.
    // A malicious workspace could pre-create .naab/naab.lock as a symlink to
    // ~/.ssh/id_rsa and trick a developer into overwriting secrets. Using
    // writeFileSecure() opens with O_NOFOLLOW so such paths fail with ELOOP.
    if (!naab::security::writeFileSecure(path, content)) {
        fprintf(stderr,
                "[lock] Refusing to write %s (symlink, permission error, or non-regular file)\n",
                path.c_str());
        return;
    }

    // V-SC-001: write HMAC-SHA256 sidecar when NAAB_LOCK_KEY is set
    const char* lock_key = std::getenv("NAAB_LOCK_KEY");
    if (lock_key && *lock_key) {
        std::string sig = naab::security::CryptoUtils::hmacSha256(content, lock_key);
        // V-SC-005: sidecar is equally sensitive — write it via the same path.
        if (!naab::security::writeFileSecure(path + ".sig", sig + "\n")) {
            fprintf(stderr,
                    "[lock] Refusing to write %s.sig (symlink or permission error)\n",
                    path.c_str());
        }
    }
}

// ============================================================================
// Lockfile::update
// ============================================================================

void Lockfile::update(const std::string& language,
                      const std::string& version,
                      const std::string& binary_path) {
    for (auto& e : runtimes) {
        if (e.language == language) {
            e.runtime_version = version;
            if (!binary_path.empty()) e.binary_path = binary_path;
            // Keep original timestamp — only update version
            return;
        }
    }
    // New entry
    LockfileEntry e;
    e.language = language;
    e.runtime_version = version;
    e.binary_path = binary_path;
    e.timestamp = currentTimestamp();
    runtimes.push_back(e);
}

// ============================================================================
// Lockfile::hasEntry / getEntry
// ============================================================================

bool Lockfile::hasEntry(const std::string& language) const {
    for (const auto& e : runtimes) {
        if (e.language == language) return true;
    }
    return false;
}

const LockfileEntry* Lockfile::getEntry(const std::string& language) const {
    for (const auto& e : runtimes) {
        if (e.language == language) return &e;
    }
    return nullptr;
}

// ============================================================================
// Lockfile::checkDrift
// ============================================================================

std::vector<std::string> Lockfile::checkDrift(
    const std::unordered_map<std::string, std::string>& observed) const
{
    std::vector<std::string> drifts;
    for (const auto& e : runtimes) {
        auto it = observed.find(e.language);
        if (it == observed.end()) continue;
        if (it->second != e.runtime_version) {
            drifts.push_back(
                "Runtime drift for " + e.language + ": "
                "locked=" + e.runtime_version + ", "
                "observed=" + it->second);
        }
    }
    return drifts;
}

// ============================================================================
// Lockfile::verifySignature
// ============================================================================

bool Lockfile::verifySignature(const std::string& lock_path) {
    const char* key_env = std::getenv("NAAB_LOCK_KEY");
    if (!key_env || !*key_env) {
        // V-SC-003: if a .sig sidecar exists, signing was previously enabled.
        // Without the key we cannot verify it — fail closed to prevent silent bypass.
        std::ifstream sig_probe(lock_path + ".sig");
        if (sig_probe.is_open()) {
            fprintf(stderr, "[lock] TAMPER: %s.sig exists but NAAB_LOCK_KEY is not set.\n"
                            "  Set NAAB_LOCK_KEY to verify, or delete the .sig to disable signing.\n",
                            lock_path.c_str());
            return false;
        }
        // No .sig and no key — signing simply not enabled; warn and proceed.
        fprintf(stderr, "[lock] Warning: NAAB_LOCK_KEY not set; lockfile integrity unverified.\n");
        return true;
    }
    // Read lock file content
    std::ifstream lf(lock_path);
    if (!lf.is_open()) return true;  // File doesn't exist yet, nothing to verify
    std::ostringstream ss; ss << lf.rdbuf();
    std::string content = ss.str();
    // Read signature sidecar
    std::ifstream sf(lock_path + ".sig");
    if (!sf.is_open()) {
        fprintf(stderr, "[lock] TAMPER: %s.sig missing but NAAB_LOCK_KEY is set.\n",
                lock_path.c_str());
        return false;
    }
    std::string stored_sig; std::getline(sf, stored_sig);
    // Compute expected HMAC and compare with constant-time comparison
    std::string expected = naab::security::CryptoUtils::hmacSha256(content, key_env);
    return naab::security::CryptoUtils::constantTimeCompare(expected, stored_sig);
}

// ============================================================================
// discoverLockfilePath
// ============================================================================

std::string discoverLockfilePath(const std::string& start_dir) {
    std::filesystem::path dir(start_dir);
    for (int depth = 0; depth < 10; ++depth) {
        std::filesystem::path candidate = dir / "govern.json";
        if (std::filesystem::exists(candidate)) {
            return (dir / ".naab" / "naab.lock").string();
        }
        std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return "";
}

} // namespace naab
