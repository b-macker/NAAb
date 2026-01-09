#pragma once

#include <string>
#include <vector>

namespace naab {
namespace security {

// Input validation and sanitization for security
class InputValidator {
public:
    // Validate block ID format: BLOCK-[A-Z]+-[0-9]+
    static bool isValidBlockId(const std::string& block_id);

    // Canonicalize path using realpath() - resolves .., symlinks, etc.
    // Returns empty string if path doesn't exist or canonicalization fails
    static std::string canonicalizePath(const std::string& path);

    // Check if path is safe (within base directory, no traversal)
    // base_path should be an absolute, canonicalized path
    static bool isSafePath(const std::string& path, const std::string& base_path);

    // Sanitize command string for shell execution
    // Removes or escapes dangerous shell metacharacters
    static std::string sanitizeCommand(const std::string& command);

    // Check if string contains dangerous shell metacharacters
    static bool hasDangerousChars(const std::string& input);

    // Validate filename (no path separators, no special chars)
    static bool isValidFilename(const std::string& filename);

    // Extract just the filename from a path
    static std::string getFilename(const std::string& path);

    // Check if path is absolute
    static bool isAbsolutePath(const std::string& path);

private:
    // Dangerous shell characters that should be escaped or rejected
    static const std::string DANGEROUS_CHARS;

    // Block ID regex pattern
    static const std::string BLOCK_ID_PATTERN;
};

} // namespace security
} // namespace naab
