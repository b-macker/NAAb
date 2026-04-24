#include "naab/crypto_utils.h"
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cstring>
#include <vector>

namespace naab {
namespace security {

std::string CryptoUtils::sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];

    SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash);

    return toHex(hash, SHA256_DIGEST_LENGTH);
}

std::string CryptoUtils::sha256File(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for hashing: " + filepath);
    }

    // Read file in chunks
    constexpr size_t BUFFER_SIZE = 8192;
    char buffer[BUFFER_SIZE];

    SHA256_CTX sha256_ctx;
    SHA256_Init(&sha256_ctx);

    while (file.read(buffer, BUFFER_SIZE) || file.gcount() > 0) {
        SHA256_Update(&sha256_ctx, buffer, file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256_ctx);

    return toHex(hash, SHA256_DIGEST_LENGTH);
}

bool CryptoUtils::verifyHash(const std::string& data, const std::string& expected_hash) {
    if (expected_hash.empty()) {
        return false;
    }

    std::string actual_hash = sha256(data);

    // Use constant-time comparison to prevent timing attacks
    return constantTimeCompare(actual_hash, expected_hash);
}

std::string CryptoUtils::toHex(const unsigned char* data, size_t length) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    for (size_t i = 0; i < length; i++) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }

    return oss.str();
}

std::string CryptoUtils::fromHex(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::invalid_argument("Hex string must have even length");
    }

    std::string result;
    result.reserve(hex.length() / 2);

    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        char byte = static_cast<char>(std::stoi(byte_str, nullptr, 16));
        result.push_back(byte);
    }

    return result;
}

std::string CryptoUtils::hmacSha256(const std::string& data, const std::string& key) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &digest_len);
    return toHex(digest, digest_len);
}

bool CryptoUtils::constantTimeCompare(const std::string& a, const std::string& b) {
    // V-API-003: do not short-circuit on length mismatch — prevents length oracle attack.
    // When i >= a.length(), compare b[i] against b[i] (always 0), keeping execution time
    // bounded by b.length() regardless of a's length. The length mismatch is recorded
    // branchlessly via result so the function still returns false when lengths differ.
    volatile unsigned char result = 0;
    size_t len = b.length();

    // Branchless length mismatch record
    result |= static_cast<unsigned char>(a.length() != len);

    for (size_t i = 0; i < len; i++) {
        unsigned char ca = (i < a.length()) ? static_cast<unsigned char>(a[i])
                                             : static_cast<unsigned char>(b[i]);
        result |= (ca ^ static_cast<unsigned char>(b[i]));
    }

    return result == 0;
}

// --- Ed25519 asymmetric signing (V-SC-009) ---

std::string CryptoUtils::toBase64(const unsigned char* data, size_t length) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(length));
    BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

std::string CryptoUtils::fromBase64(const std::string& b64) {
    BIO* bio = BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()));
    BIO* b64f = BIO_new(BIO_f_base64());
    bio = BIO_push(b64f, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    std::vector<unsigned char> buf(b64.size());
    int len = BIO_read(bio, buf.data(), static_cast<int>(buf.size()));
    BIO_free_all(bio);
    if (len < 0) return "";
    return std::string(reinterpret_cast<char*>(buf.data()), static_cast<size_t>(len));
}

bool CryptoUtils::ed25519Keygen(std::string& out_private_pem, std::string& out_public_pem) {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) return false;

    bool ok = false;
    if (EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0) {
        // Write private key PEM
        BIO* priv_bio = BIO_new(BIO_s_mem());
        if (PEM_write_bio_PrivateKey(priv_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr)) {
            BUF_MEM* bptr = nullptr;
            BIO_get_mem_ptr(priv_bio, &bptr);
            out_private_pem.assign(bptr->data, bptr->length);

            // Write public key PEM
            BIO* pub_bio = BIO_new(BIO_s_mem());
            if (PEM_write_bio_PUBKEY(pub_bio, pkey)) {
                BUF_MEM* pbptr = nullptr;
                BIO_get_mem_ptr(pub_bio, &pbptr);
                out_public_pem.assign(pbptr->data, pbptr->length);
                ok = true;
            }
            BIO_free(pub_bio);
        }
        BIO_free(priv_bio);
    }

    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

std::string CryptoUtils::ed25519Sign(const std::string& data, const std::string& private_key_pem) {
    BIO* bio = BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size()));
    if (!bio) return "";

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) return "";

    // Verify it's Ed25519
    if (EVP_PKEY_id(pkey) != EVP_PKEY_ED25519) {
        EVP_PKEY_free(pkey);
        return "";
    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) { EVP_PKEY_free(pkey); return ""; }

    std::string result;
    if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, pkey) > 0) {
        // First call to get signature length
        size_t sig_len = 0;
        if (EVP_DigestSign(md_ctx, nullptr, &sig_len,
                           reinterpret_cast<const unsigned char*>(data.data()), data.size()) > 0) {
            std::vector<unsigned char> sig(sig_len);
            if (EVP_DigestSign(md_ctx, sig.data(), &sig_len,
                               reinterpret_cast<const unsigned char*>(data.data()), data.size()) > 0) {
                result = toBase64(sig.data(), sig_len);
            }
        }
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return result;
}

bool CryptoUtils::ed25519Verify(const std::string& data, const std::string& signature_b64,
                                 const std::string& public_key_pem) {
    std::string sig_raw = fromBase64(signature_b64);
    if (sig_raw.empty()) return false;

    BIO* bio = BIO_new_mem_buf(public_key_pem.data(), static_cast<int>(public_key_pem.size()));
    if (!bio) return false;

    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) return false;  // Reject non-public-key PEM (do NOT fall back to private key)

    if (EVP_PKEY_id(pkey) != EVP_PKEY_ED25519) {
        EVP_PKEY_free(pkey);
        return false;
    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) { EVP_PKEY_free(pkey); return false; }

    bool ok = false;
    if (EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) > 0) {
        int rc = EVP_DigestVerify(md_ctx,
                                  reinterpret_cast<const unsigned char*>(sig_raw.data()), sig_raw.size(),
                                  reinterpret_cast<const unsigned char*>(data.data()), data.size());
        ok = (rc == 1);
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

std::string CryptoUtils::ed25519Fingerprint(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return "";

    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        // Try as private key
        BIO* bio2 = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        pkey = PEM_read_bio_PrivateKey(bio2, nullptr, nullptr, nullptr);
        BIO_free(bio2);
        if (!pkey) return "";
    }

    // Extract raw public key bytes (32 bytes for Ed25519)
    size_t raw_len = 0;
    if (EVP_PKEY_get_raw_public_key(pkey, nullptr, &raw_len) <= 0) {
        EVP_PKEY_free(pkey);
        return "";
    }
    std::vector<unsigned char> raw(raw_len);
    if (EVP_PKEY_get_raw_public_key(pkey, raw.data(), &raw_len) <= 0) {
        EVP_PKEY_free(pkey);
        return "";
    }
    EVP_PKEY_free(pkey);

    // SHA-256 of raw public key bytes
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(raw.data(), raw_len, hash);

    std::string full_hex = toHex(hash, SHA256_DIGEST_LENGTH);
    // Return last 16 hex chars as fingerprint
    return full_hex.substr(full_hex.size() > 16 ? full_hex.size() - 16 : 0);
}

} // namespace security
} // namespace naab
