#include "naab/input_validator.h"
#include <regex>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <unistd.h>

namespace naab {
namespace security {

const std::string InputValidator::DANGEROUS_CHARS = "|&;`$()<>\\\"'*?[]{}!~";
const std::string InputValidator::BLOCK_ID_PATTERN = "^BLOCK-[A-Z]+-[0-9]+$";

bool InputValidator::isValidBlockId(const std::string& block_id) {
    if (block_id.empty()) {
        return false;
    }

    // Check pattern: BLOCK-[A-Z]+-[0-9]+
    std::regex pattern(BLOCK_ID_PATTERN);
    return std::regex_match(block_id, pattern);
}

std::string InputValidator::canonicalizePath(const std::string& path) {
    if (path.empty()) {
        return "";
    }

    char resolved_path[PATH_MAX];
    char* result = realpath(path.c_str(), resolved_path);

    if (result == nullptr) {
        // Path doesn't exist or canonicalization failed
        return "";
    }

    return std::string(resolved_path);
}

bool InputValidator::isSafePath(const std::string& path, const std::string& base_path) {
    if (path.empty() || base_path.empty()) {
        return false;
    }

    // Canonicalize the input path
    std::string canonical_path = canonicalizePath(path);
    if (canonical_path.empty()) {
        // Path doesn't exist or can't be canonicalized
        // For security, we might want to allow non-existent paths
        // if they're being created, so let's check the parent instead
        size_t last_slash = path.find_last_of('/');
        if (last_slash != std::string::npos) {
            std::string parent = path.substr(0, last_slash);
            canonical_path = canonicalizePath(parent);
            if (canonical_path.empty()) {
                return false;
            }
            // Append the filename back
            canonical_path += "/" + path.substr(last_slash + 1);
        } else {
            return false;
        }
    }

    // Ensure base_path ends with /
    std::string base = base_path;
    if (!base.empty() && base.back() != '/') {
        base += '/';
    }

    // Check if canonical_path starts with base_path
    if (canonical_path.size() < base.size()) {
        return false;
    }

    return canonical_path.compare(0, base.size(), base) == 0;
}

std::string InputValidator::sanitizeCommand(const std::string& command) {
    std::string sanitized;
    sanitized.reserve(command.size() * 2);  // Reserve extra space for escaping

    for (char c : command) {
        // Check if character is dangerous
        if (DANGEROUS_CHARS.find(c) != std::string::npos) {
            // Escape with backslash
            sanitized += '\\';
        }
        sanitized += c;
    }

    return sanitized;
}

bool InputValidator::hasDangerousChars(const std::string& input) {
    return input.find_first_of(DANGEROUS_CHARS) != std::string::npos;
}

bool InputValidator::isValidFilename(const std::string& filename) {
    if (filename.empty()) {
        return false;
    }

    // Reject path separators
    if (filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
        return false;
    }

    // Reject parent directory references
    if (filename == "." || filename == "..") {
        return false;
    }

    // Reject null bytes
    if (filename.find('\0') != std::string::npos) {
        return false;
    }

    // Reject control characters
    for (char c : filename) {
        if (c < 32 || c == 127) {
            return false;
        }
    }

    return true;
}

std::string InputValidator::getFilename(const std::string& path) {
    size_t last_slash = path.find_last_of('/');
    if (last_slash == std::string::npos) {
        return path;
    }
    return path.substr(last_slash + 1);
}

bool InputValidator::isAbsolutePath(const std::string& path) {
    return !path.empty() && path[0] == '/';
}

} // namespace security
} // namespace naab
