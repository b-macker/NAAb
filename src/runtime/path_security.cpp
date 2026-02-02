// NAAb Path Security Implementation
// Week 4, Task 4.2: Path Canonicalization

#include "naab/path_security.h"
#include <fmt/core.h>
#include <algorithm>

namespace naab {
namespace security {

// Static member initialization
std::vector<std::filesystem::path> PathSecurity::allowed_directories_;

// ============================================================================
// Path Canonicalization
// ============================================================================

std::filesystem::path PathSecurity::canonicalize(
    const std::string& path,
    bool allow_absolute
) {
    // Check for dangerous patterns first
    checkDangerousPatterns(path);

    // Convert to filesystem path
    std::filesystem::path fs_path(path);

    // Check if absolute path is allowed
    if (fs_path.is_absolute() && !allow_absolute) {
        throw PathSecurityException(
            fmt::format("Absolute paths not allowed: {}", path)
        );
    }

    // Check for traversal before canonicalizing
    checkPathTraversal(fs_path);

    // Canonicalize the path
    std::filesystem::path canonical;
    try {
        // If path exists, use canonical (resolves symlinks)
        if (std::filesystem::exists(fs_path)) {
            canonical = std::filesystem::canonical(fs_path);
        } else {
            // For non-existent paths, use weakly_canonical
            // (resolves .. and . but doesn't require existence)
            canonical = std::filesystem::weakly_canonical(fs_path);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        throw PathSecurityException(
            fmt::format("Failed to canonicalize path '{}': {}", path, e.what())
        );
    }

    // Double-check for traversal after canonicalization
    checkPathTraversal(canonical);

    return canonical;
}

// ============================================================================
// Traversal Detection
// ============================================================================

void PathSecurity::checkPathTraversal(const std::filesystem::path& path) {
    std::string path_str = path.string();

    // Check for null bytes
    if (path_str.find('\0') != std::string::npos) {
        throw PathSecurityException("Path contains null bytes");
    }

    // Check for parent directory references
    // Note: After canonicalization, these should be resolved,
    // but we check anyway as defense in depth
    for (const auto& component : path) {
        std::string comp_str = component.string();

        // Check for ".." (parent directory traversal)
        if (comp_str == "..") {
            throw PathSecurityException(
                fmt::format("Path traversal attempt detected: {}", path.string())
            );
        }
    }
}

// ============================================================================
// Base Directory Validation
// ============================================================================

bool PathSecurity::isPathSafe(
    const std::filesystem::path& path,
    const std::filesystem::path& base_dir
) {
    if (base_dir.empty()) {
        return true;  // No restriction
    }

    try {
        // Get canonical paths
        std::filesystem::path canonical_path = std::filesystem::weakly_canonical(path);
        std::filesystem::path canonical_base = std::filesystem::canonical(base_dir);

        // Check if path starts with base_dir
        auto path_str = canonical_path.string();
        auto base_str = canonical_base.string();

        // Ensure base_str ends with separator for exact prefix match
        if (!base_str.empty() && base_str.back() != std::filesystem::path::preferred_separator) {
            base_str += std::filesystem::path::preferred_separator;
        }

        return path_str.find(base_str) == 0;
    } catch (const std::filesystem::filesystem_error&) {
        // If we can't canonicalize, reject for safety
        return false;
    }
}

// ============================================================================
// File Path Validation
// ============================================================================

std::filesystem::path PathSecurity::validateFilePath(
    const std::string& path,
    const std::filesystem::path& base_dir
) {
    // Canonicalize the path
    auto canonical = canonicalize(path, true);

    // Check against base directory if specified
    if (!base_dir.empty()) {
        if (!isPathSafe(canonical, base_dir)) {
            throw PathSecurityException(
                fmt::format("Path '{}' is outside allowed directory '{}'",
                           canonical.string(), base_dir.string())
            );
        }
    }

    // Check against allowed directories list
    if (!allowed_directories_.empty()) {
        bool is_allowed = false;
        for (const auto& allowed_dir : allowed_directories_) {
            if (isPathSafe(canonical, allowed_dir)) {
                is_allowed = true;
                break;
            }
        }

        if (!is_allowed) {
            throw PathSecurityException(
                fmt::format("Path '{}' is not in any allowed directory",
                           canonical.string())
            );
        }
    }

    return canonical;
}

// ============================================================================
// Allowed Directories Management
// ============================================================================

std::vector<std::filesystem::path> PathSecurity::getAllowedDirectories() {
    if (allowed_directories_.empty()) {
        // Default: current working directory and /tmp
        return {
            std::filesystem::current_path(),
            std::filesystem::path("/tmp")
        };
    }
    return allowed_directories_;
}

void PathSecurity::setAllowedDirectories(
    const std::vector<std::filesystem::path>& dirs
) {
    allowed_directories_ = dirs;
}

// ============================================================================
// Dangerous Pattern Detection
// ============================================================================

void PathSecurity::checkDangerousPatterns(const std::string& path) {
    // Check for null bytes
    if (path.find('\0') != std::string::npos) {
        throw PathSecurityException("Path contains null bytes");
    }

    // Check for control characters
    for (char c : path) {
        if (c > 0 && c < 32 && c != '\t' && c != '\n' && c != '\r') {
            throw PathSecurityException(
                fmt::format("Path contains control character: 0x{:02x}", (unsigned char)c)
            );
        }
    }

    // Check for shell metacharacters (if path might be used in shell commands)
    // Note: This is extra paranoid; ideally paths should never go to shell
    const std::string dangerous_chars = ";|&$`<>(){}[]!*?";
    for (char dangerous : dangerous_chars) {
        if (path.find(dangerous) != std::string::npos) {
            // This is a warning, not an error, since these chars can be valid in filenames
            // But they should be carefully handled if ever passed to shell
            // For now, we allow them but log a warning
            // fmt::print(stderr, "Warning: Path contains shell metacharacter: {}\n", dangerous);
        }
    }
}

// ============================================================================
// Path Resolution
// ============================================================================

std::filesystem::path PathSecurity::resolvePath(
    const std::filesystem::path& base,
    const std::string& relative
) {
    // Check relative path for dangerous patterns
    checkDangerousPatterns(relative);

    // Prevent absolute paths being passed as "relative"
    std::filesystem::path rel_path(relative);
    if (rel_path.is_absolute()) {
        throw PathSecurityException(
            fmt::format("Absolute path passed as relative: {}", relative)
        );
    }

    // Join paths
    std::filesystem::path joined = base / rel_path;

    // Canonicalize and validate
    return validateFilePath(joined.string(), base);
}

} // namespace security
} // namespace naab
