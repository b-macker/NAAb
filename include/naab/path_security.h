// NAAb Path Security
// Week 4, Task 4.2: Path Canonicalization and Traversal Prevention
//
// Prevents path traversal attacks by:
// - Canonicalizing all file paths
// - Checking for directory traversal attempts (../)
// - Validating paths are within allowed directories
// - Preventing symlink attacks

#ifndef NAAB_PATH_SECURITY_H
#define NAAB_PATH_SECURITY_H

#include <string>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace naab {
namespace security {

// ============================================================================
// Path Security Exceptions
// ============================================================================

class PathSecurityException : public std::runtime_error {
public:
    explicit PathSecurityException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// ============================================================================
// Path Security
// ============================================================================

class PathSecurity {
public:
    /**
     * Canonicalize a file path and check for security issues
     *
     * - Resolves symbolic links
     * - Removes . and .. components
     * - Converts to absolute path
     * - Checks for directory traversal attempts
     *
     * @param path The path to canonicalize
     * @param allow_absolute If false, reject absolute paths
     * @return Canonicalized path
     * @throws PathSecurityException if path is unsafe
     */
    static std::filesystem::path canonicalize(
        const std::string& path,
        bool allow_absolute = true
    );

    /**
     * Check if a path is safe (no traversal attempts)
     *
     * Checks for:
     * - ../ components (directory traversal)
     * - Absolute paths (when not allowed)
     * - Null bytes
     * - Invalid characters
     */
    static void checkPathTraversal(const std::filesystem::path& path);

    /**
     * Check if a path is within an allowed base directory
     *
     * Prevents accessing files outside the allowed directory tree
     *
     * @param path The path to check
     * @param base_dir The allowed base directory
     * @return true if path is within base_dir
     */
    static bool isPathSafe(
        const std::filesystem::path& path,
        const std::filesystem::path& base_dir
    );

    /**
     * Validate file path before opening
     *
     * - Canonicalizes path
     * - Checks for traversal
     * - Optionally checks if within base directory
     *
     * @param path The path to validate
     * @param base_dir Optional base directory restriction
     * @return Canonicalized safe path
     */
    static std::filesystem::path validateFilePath(
        const std::string& path,
        const std::filesystem::path& base_dir = ""
    );

    /**
     * Get allowed base directories for file operations
     *
     * By default, only allow access to:
     * - Current working directory
     * - /tmp (for temporary files)
     * - User-specified directories
     */
    static std::vector<std::filesystem::path> getAllowedDirectories();

    /**
     * Set allowed base directories (for sandboxing)
     */
    static void setAllowedDirectories(
        const std::vector<std::filesystem::path>& dirs
    );

    /**
     * Check if a path contains dangerous patterns
     *
     * Checks for:
     * - Null bytes
     * - Control characters
     * - Shell metacharacters (if shell execution is possible)
     */
    static void checkDangerousPatterns(const std::string& path);

    /**
     * Resolve path relative to base directory
     *
     * Safely joins base and relative path, preventing traversal
     */
    static std::filesystem::path resolvePath(
        const std::filesystem::path& base,
        const std::string& relative
    );

private:
    // Allowed base directories (empty = unrestricted)
    static std::vector<std::filesystem::path> allowed_directories_;
};

// ============================================================================
// RAII Guard for Path Validation
// ============================================================================

class PathValidationGuard {
public:
    explicit PathValidationGuard(
        const std::string& path,
        const std::filesystem::path& base_dir = ""
    ) : validated_path_(PathSecurity::validateFilePath(path, base_dir)) {}

    const std::filesystem::path& path() const {
        return validated_path_;
    }

    operator std::filesystem::path() const {
        return validated_path_;
    }

private:
    std::filesystem::path validated_path_;
};

} // namespace security
} // namespace naab

#endif // NAAB_PATH_SECURITY_H
