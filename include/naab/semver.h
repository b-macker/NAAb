#ifndef NAAB_SEMVER_H
#define NAAB_SEMVER_H

// NAAb Semantic Versioning Library
// Implements semver.org specification 2.0.0

#include <string>
#include <stdexcept>

namespace naab {
namespace versioning {

// Exception thrown when version parsing fails
class VersionParseException : public std::runtime_error {
public:
    explicit VersionParseException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// Semantic Version Structure
struct SemanticVersion {
    int major;
    int minor;
    int patch;
    std::string prerelease;      // e.g., "alpha.1", "beta.2", "rc.1"
    std::string build_metadata;  // e.g., "20241227.abc1234"

    // Constructors
    SemanticVersion();
    SemanticVersion(int maj, int min, int pat,
                   const std::string& pre = "",
                   const std::string& build = "");

    // Parse version string (e.g., "1.2.3-alpha.1+build.123")
    static SemanticVersion parse(const std::string& version_str);

    // Convert to string representation
    std::string toString() const;
    std::string toStringWithBuild() const;

    // Comparison operators (follow semver precedence rules)
    bool operator<(const SemanticVersion& other) const;
    bool operator>(const SemanticVersion& other) const;
    bool operator==(const SemanticVersion& other) const;
    bool operator!=(const SemanticVersion& other) const;
    bool operator<=(const SemanticVersion& other) const;
    bool operator>=(const SemanticVersion& other) const;

    // Compatibility checks
    bool isCompatibleWith(const SemanticVersion& required) const;

    // Range satisfaction (e.g., ">=1.0.0,<2.0.0", "^1.2.3", "~1.2.3")
    bool satisfiesRange(const std::string& range) const;

private:
    // Helper: Compare prerelease versions
    static int comparePrereleaseIdentifiers(const std::string& a, const std::string& b);
};

// Compatibility status enum
enum class Compatibility {
    COMPATIBLE,           // Same major, minor >= required
    BREAKING_CHANGE,      // Major version mismatch
    FEATURE_MISSING,      // Minor version < required
    PATCH_OUTDATED        // Only patch differs
};

// Check compatibility between two versions
Compatibility checkCompatibility(const SemanticVersion& current,
                                 const SemanticVersion& required);

// Get human-readable compatibility message
std::string getCompatibilityMessage(Compatibility compat,
                                   const SemanticVersion& current,
                                   const SemanticVersion& required);

} // namespace versioning
} // namespace naab

#endif // NAAB_SEMVER_H
