#pragma once
//
// NAAb Lockfile — naab.lock
// Records observed polyglot runtime versions for reproducible builds.
// Written to .naab/naab.lock alongside govern.json.
//
// Usage:
//   Lockfile lf = Lockfile::load(".naab/naab.lock");
//   lf.update("python", "Python 3.11.2", "/usr/bin/python3");
//   lf.save(".naab/naab.lock");
//

#include <string>
#include <vector>
#include <unordered_map>

namespace naab {

struct LockfileEntry {
    std::string language;
    std::string runtime_version;   // Full observed version, e.g., "Python 3.11.2"
    std::string binary_path;       // Path to the runtime binary
    std::string timestamp;         // ISO 8601 timestamp when first recorded
};

struct Lockfile {
    std::string naab_version;      // NAAb version string (e.g., "0.7.0")
    std::string platform;          // "linux/arm64", "darwin/arm64", etc.
    std::vector<LockfileEntry> runtimes;
    bool parse_failed = false;     // True if lockfile JSON was malformed

    // Load from file. Returns empty Lockfile if file doesn't exist.
    static Lockfile load(const std::string& path);

    // Save to file (creates parent directory if needed).
    void save(const std::string& path) const;

    // Update or add an entry for a language. Updates timestamp only on first record.
    void update(const std::string& language,
                const std::string& version,
                const std::string& binary_path = "");

    bool hasEntry(const std::string& language) const;
    const LockfileEntry* getEntry(const std::string& language) const;

    // Check observed versions against this lockfile. Returns list of drift messages.
    // Empty = no drift, all runtimes match.
    std::vector<std::string> checkDrift(
        const std::unordered_map<std::string, std::string>& observed) const;

    // Verify the HMAC-SHA256 signature sidecar for a lockfile.
    // Returns true if NAAB_LOCK_KEY is absent (unverified, warns) OR if signature matches.
    // Returns false (tamper detected) if key is set but signature is wrong or missing.
    static bool verifySignature(const std::string& lock_path);
};

// Discover lockfile path: looks for .naab/naab.lock alongside the nearest govern.json.
// Returns empty string if no govern.json found.
std::string discoverLockfilePath(const std::string& start_dir);

} // namespace naab
