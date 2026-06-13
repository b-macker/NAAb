#include "naab/trust_store.h"
#include "naab/crypto_utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#ifndef _WIN32
#  include <sys/stat.h>
#endif

namespace naab {
namespace security {

std::string TrustStore::getStorePath() {
    // Allow test isolation via explicit override (not in LLM-facing docs)
    const char* override_dir = std::getenv("NAAB_TRUST_STORE_DIR");
    if (override_dir && *override_dir) return std::string(override_dir);

    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    if (!home || !*home) {
        home = "/tmp";
    }
    return std::string(home) + "/.naab/trusted-keys";
}

bool TrustStore::hasKeys() {
    std::string store = getStorePath();
    std::error_code ec;
    if (!std::filesystem::is_directory(store, ec)) return false;

    for (const auto& entry : std::filesystem::directory_iterator(store, ec)) {
        if (ec) break;
        if (entry.path().extension() == ".pub" && entry.is_regular_file(ec)) {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> TrustStore::loadKeys() {
    std::vector<std::pair<std::string, std::string>> keys;
    std::string store = getStorePath();
    std::error_code ec;
    if (!std::filesystem::is_directory(store, ec)) return keys;

    for (const auto& entry : std::filesystem::directory_iterator(store, ec)) {
        if (ec) break;
        if (entry.path().extension() != ".pub" || !entry.is_regular_file(ec)) continue;

        // Skip default.pub (it's a copy of another key, avoid duplicate verification)
        if (entry.path().filename() == "default.pub") continue;

        // Size cap: Ed25519 PEM public keys are ~120 bytes; reject > 8KB
        auto fsize = entry.file_size(ec);
        if (ec || fsize > 8192) {
            fprintf(stderr, "[trust-store] WARNING: Key file too large or unreadable, skipping: %s\n",
                    entry.path().string().c_str());
            continue;
        }

        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) {
            fprintf(stderr, "[trust-store] WARNING: Cannot read key file: %s\n",
                    entry.path().string().c_str());
            continue;
        }
        std::string pem((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
        ifs.close();

        std::string fp = CryptoUtils::ed25519Fingerprint(pem);
        if (fp.empty()) {
            fprintf(stderr, "[trust-store] WARNING: Invalid key file (not Ed25519 PEM): %s\n",
                    entry.path().string().c_str());
            continue;
        }

        // Skip expired/revoked keys
        KeyMetadata meta = loadKeyMetadata(fp);
        if (isKeyExpired(meta)) {
            fprintf(stderr, "[trust-store] WARNING: Skipping expired key: %s\n", fp.c_str());
            continue;
        }
        if (meta.revoked) {
            fprintf(stderr, "[trust-store] WARNING: Skipping revoked key: %s (%s)\n",
                    fp.c_str(), meta.revoked_reason.c_str());
            continue;
        }

        keys.emplace_back(fp, pem);
    }
    return keys;
}

bool TrustStore::installKey(const std::string& public_key_pem) {
    // Reject private keys — only public keys belong in the trust store
    if (public_key_pem.find("PRIVATE KEY") != std::string::npos) {
        fprintf(stderr, "[trust-store] Error: This is a private key — only public keys can be installed.\n"
                        "  Private keys should be kept secret and never placed in the trust store.\n");
        return false;
    }
    // Validate it's a real Ed25519 public key
    std::string fp = CryptoUtils::ed25519Fingerprint(public_key_pem);
    if (fp.empty()) {
        fprintf(stderr, "[trust-store] Error: Invalid Ed25519 public key PEM\n");
        return false;
    }

    std::string store = getStorePath();
    std::error_code ec;
    std::filesystem::create_directories(store, ec);
    if (ec) {
        fprintf(stderr, "[trust-store] Error: Cannot create trust store at %s: %s\n",
                store.c_str(), ec.message().c_str());
        return false;
    }

    // Set directory permissions to 0755 (POSIX only)
#ifndef _WIN32
    chmod(store.c_str(), 0755);
#endif

    // Write <fingerprint>.pub
    std::string key_path = store + "/" + fp + ".pub";
    std::ofstream ofs(key_path);
    if (!ofs.is_open()) {
        fprintf(stderr, "[trust-store] Error: Cannot write key file: %s\n", key_path.c_str());
        return false;
    }
    ofs << public_key_pem;
    ofs.close();
#ifndef _WIN32
    chmod(key_path.c_str(), 0644);
#endif

    // Stamp key metadata (created_at) if no metadata exists yet
    KeyMetadata existing_meta = loadKeyMetadata(fp);
    if (existing_meta.created_at == 0) {
        KeyMetadata meta;
        meta.fingerprint = fp;
        meta.created_at = static_cast<int64_t>(std::time(nullptr));
        saveKeyMetadata(fp, meta);
    }

    // Create default.pub if it doesn't exist (first key installed)
    std::string default_path = store + "/default.pub";
    if (!std::filesystem::exists(default_path, ec)) {
        std::ofstream dfs(default_path);
        if (dfs.is_open()) {
            dfs << public_key_pem;
            dfs.close();
#ifndef _WIN32
            chmod(default_path.c_str(), 0644);
#endif
        }
    }

    return true;
}

std::vector<std::string> TrustStore::listKeyFingerprints() {
    std::vector<std::string> fingerprints;
    auto keys = loadKeys();
    for (const auto& [fp, pem] : keys) {
        fingerprints.push_back(fp);
    }
    return fingerprints;
}

// ============================================================================
// Key Metadata (Authority Decay)
// ============================================================================

KeyMetadata TrustStore::loadKeyMetadata(const std::string& fingerprint) {
    KeyMetadata meta;
    meta.fingerprint = fingerprint;
    std::string meta_path = getStorePath() + "/" + fingerprint + ".meta.json";
    std::ifstream ifs(meta_path);
    if (!ifs.is_open()) return meta;  // No metadata file — backward compat

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (j.contains("fingerprint") && j["fingerprint"].is_string()) meta.fingerprint = j["fingerprint"].get<std::string>();
        if (j.contains("label") && j["label"].is_string()) meta.label = j["label"].get<std::string>();
        if (j.contains("created_at")) meta.created_at = j["created_at"].get<int64_t>();
        if (j.contains("expires_at")) meta.expires_at = j["expires_at"].get<int64_t>();
        if (j.contains("revoked")) meta.revoked = j["revoked"].get<bool>();
        if (j.contains("revoked_reason") && j["revoked_reason"].is_string()) meta.revoked_reason = j["revoked_reason"].get<std::string>();
        if (j.contains("revoked_at")) meta.revoked_at = j["revoked_at"].get<int64_t>();
    } catch (const std::exception& e) {
        fprintf(stderr, "[trust-store] WARNING: Invalid metadata for key %s: %s\n",
                fingerprint.c_str(), e.what());
    }
    return meta;
}

bool TrustStore::saveKeyMetadata(const std::string& fingerprint, const KeyMetadata& meta) {
    std::string store = getStorePath();
    std::error_code ec;
    std::filesystem::create_directories(store, ec);
    if (ec) return false;

    nlohmann::json j;
    j["fingerprint"] = meta.fingerprint.empty() ? fingerprint : meta.fingerprint;
    j["label"] = meta.label;
    j["created_at"] = meta.created_at;
    j["expires_at"] = meta.expires_at;
    j["revoked"] = meta.revoked;
    j["revoked_reason"] = meta.revoked_reason;
    j["revoked_at"] = meta.revoked_at;

    std::string meta_path = store + "/" + fingerprint + ".meta.json";
    std::ofstream ofs(meta_path);
    if (!ofs.is_open()) {
        fprintf(stderr, "[trust-store] Error: Cannot write metadata: %s\n", meta_path.c_str());
        return false;
    }
    ofs << j.dump(2) << "\n";
    ofs.close();
#ifndef _WIN32
    chmod(meta_path.c_str(), 0644);
#endif
    return true;
}

std::vector<KeyMetadata> TrustStore::loadKeysWithMetadata() {
    std::vector<KeyMetadata> result;
    auto keys = loadKeys();
    for (const auto& [fp, pem] : keys) {
        result.push_back(loadKeyMetadata(fp));
    }
    return result;
}

bool TrustStore::isKeyExpired(const KeyMetadata& meta) {
    return meta.expires_at > 0 && std::time(nullptr) > meta.expires_at;
}

bool TrustStore::isKeyRevoked(const std::string& fingerprint) {
    KeyMetadata meta = loadKeyMetadata(fingerprint);
    return meta.revoked;
}

bool TrustStore::revokeKey(const std::string& fingerprint, const std::string& reason) {
    // Verify key exists
    std::string key_path = getStorePath() + "/" + fingerprint + ".pub";
    std::error_code ec;
    if (!std::filesystem::exists(key_path, ec)) {
        fprintf(stderr, "[trust-store] Error: Key not found: %s\n", fingerprint.c_str());
        return false;
    }

    KeyMetadata meta = loadKeyMetadata(fingerprint);
    meta.fingerprint = fingerprint;
    meta.revoked = true;
    meta.revoked_reason = reason;
    meta.revoked_at = static_cast<int64_t>(std::time(nullptr));
    return saveKeyMetadata(fingerprint, meta);
}

} // namespace security
} // namespace naab
