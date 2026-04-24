#include "naab/trust_store.h"
#include "naab/crypto_utils.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>

namespace naab {
namespace security {

std::string TrustStore::getStorePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        // Fallback for edge cases
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
                    entry.path().c_str());
            continue;
        }

        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) {
            fprintf(stderr, "[trust-store] WARNING: Cannot read key file: %s\n",
                    entry.path().c_str());
            continue;
        }
        std::string pem((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
        ifs.close();

        std::string fp = CryptoUtils::ed25519Fingerprint(pem);
        if (fp.empty()) {
            fprintf(stderr, "[trust-store] WARNING: Invalid key file (not Ed25519 PEM): %s\n",
                    entry.path().c_str());
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
                        "  Private keys should be kept secret. Use --keygen to generate a keypair.\n");
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

    // Set directory permissions to 0755
    chmod(store.c_str(), 0755);

    // Write <fingerprint>.pub
    std::string key_path = store + "/" + fp + ".pub";
    std::ofstream ofs(key_path);
    if (!ofs.is_open()) {
        fprintf(stderr, "[trust-store] Error: Cannot write key file: %s\n", key_path.c_str());
        return false;
    }
    ofs << public_key_pem;
    ofs.close();
    chmod(key_path.c_str(), 0644);

    // Create default.pub if it doesn't exist (first key installed)
    std::string default_path = store + "/default.pub";
    if (!std::filesystem::exists(default_path, ec)) {
        std::ofstream dfs(default_path);
        if (dfs.is_open()) {
            dfs << public_key_pem;
            dfs.close();
            chmod(default_path.c_str(), 0644);
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

} // namespace security
} // namespace naab
