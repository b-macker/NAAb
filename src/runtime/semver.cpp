// NAAb Semantic Versioning Implementation
// Implements semver.org specification 2.0.0

#include "naab/semver.h"
#include <fmt/core.h>
#include <regex>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace naab {
namespace versioning {

// Constructors
SemanticVersion::SemanticVersion()
    : major(0), minor(0), patch(0), prerelease(""), build_metadata("") {}

SemanticVersion::SemanticVersion(int maj, int min, int pat,
                                 const std::string& pre,
                                 const std::string& build)
    : major(maj), minor(min), patch(pat), prerelease(pre), build_metadata(build) {}

// Parse semantic version string
SemanticVersion SemanticVersion::parse(const std::string& version_str) {
    // Regex pattern for semver: MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
    // Pattern: ^(\d+)\.(\d+)\.(\d+)(?:-([a-zA-Z0-9.-]+))?(?:\+([a-zA-Z0-9.-]+))?$
    std::regex semver_pattern(
        R"(^(\d+)\.(\d+)\.(\d+)(?:-([a-zA-Z0-9.-]+))?(?:\+([a-zA-Z0-9.-]+))?$)"
    );

    std::smatch matches;
    if (!std::regex_match(version_str, matches, semver_pattern)) {
        throw VersionParseException(
            fmt::format("Invalid semantic version string: '{}'", version_str));
    }

    // Extract components
    int major = std::stoi(matches[1].str());
    int minor = std::stoi(matches[2].str());
    int patch = std::stoi(matches[3].str());
    std::string prerelease = matches[4].str();
    std::string build_metadata = matches[5].str();

    return SemanticVersion(major, minor, patch, prerelease, build_metadata);
}

// Convert to string
std::string SemanticVersion::toString() const {
    std::string result = fmt::format("{}.{}.{}", major, minor, patch);

    if (!prerelease.empty()) {
        result += "-" + prerelease;
    }

    return result;
}

std::string SemanticVersion::toStringWithBuild() const {
    std::string result = toString();

    if (!build_metadata.empty()) {
        result += "+" + build_metadata;
    }

    return result;
}

// Comparison operators (per semver spec section 11)
bool SemanticVersion::operator==(const SemanticVersion& other) const {
    // Build metadata is ignored in comparisons per semver spec
    return major == other.major &&
           minor == other.minor &&
           patch == other.patch &&
           prerelease == other.prerelease;
}

bool SemanticVersion::operator!=(const SemanticVersion& other) const {
    return !(*this == other);
}

bool SemanticVersion::operator<(const SemanticVersion& other) const {
    // 1. Compare major.minor.patch first
    if (major != other.major) return major < other.major;
    if (minor != other.minor) return minor < other.minor;
    if (patch != other.patch) return patch < other.patch;

    // 2. When major.minor.patch are equal, pre-release version has lower precedence
    // Per semver spec: 1.0.0-alpha < 1.0.0
    if (prerelease.empty() && !other.prerelease.empty()) return false;
    if (!prerelease.empty() && other.prerelease.empty()) return true;

    // 3. Compare prerelease identifiers if both have them
    if (!prerelease.empty() && !other.prerelease.empty()) {
        return comparePrereleaseIdentifiers(prerelease, other.prerelease) < 0;
    }

    return false; // Equal
}

bool SemanticVersion::operator>(const SemanticVersion& other) const {
    return other < *this;
}

bool SemanticVersion::operator<=(const SemanticVersion& other) const {
    return !(other < *this);
}

bool SemanticVersion::operator>=(const SemanticVersion& other) const {
    return !(*this < other);
}

// Compare prerelease identifiers per semver spec section 11.4
int SemanticVersion::comparePrereleaseIdentifiers(const std::string& a, const std::string& b) {
    // Split by dots
    std::vector<std::string> a_parts, b_parts;

    std::stringstream ss_a(a);
    std::string token;
    while (std::getline(ss_a, token, '.')) {
        a_parts.push_back(token);
    }

    std::stringstream ss_b(b);
    while (std::getline(ss_b, token, '.')) {
        b_parts.push_back(token);
    }

    // Compare identifiers one by one
    size_t min_size = std::min(a_parts.size(), b_parts.size());

    for (size_t i = 0; i < min_size; ++i) {
        const std::string& a_id = a_parts[i];
        const std::string& b_id = b_parts[i];

        // Check if both are numeric
        bool a_numeric = !a_id.empty() && std::all_of(a_id.begin(), a_id.end(), ::isdigit);
        bool b_numeric = !b_id.empty() && std::all_of(b_id.begin(), b_id.end(), ::isdigit);

        if (a_numeric && b_numeric) {
            // Both numeric: compare as integers
            int a_num = std::stoi(a_id);
            int b_num = std::stoi(b_id);
            if (a_num != b_num) return (a_num < b_num) ? -1 : 1;
        } else if (a_numeric) {
            // Numeric identifiers have lower precedence than alphanumeric
            return -1;
        } else if (b_numeric) {
            return 1;
        } else {
            // Both alphanumeric: compare lexically (ASCII)
            if (a_id != b_id) return (a_id < b_id) ? -1 : 1;
        }
    }

    // If all identifiers equal, larger set wins
    if (a_parts.size() < b_parts.size()) return -1;
    if (a_parts.size() > b_parts.size()) return 1;
    return 0;
}

// Check if compatible with required version (same major, minor >= required)
bool SemanticVersion::isCompatibleWith(const SemanticVersion& required) const {
    // Major version must match (breaking changes)
    if (major != required.major) return false;

    // Minor version must be >= required (backward compatible)
    if (minor < required.minor) return false;

    // If minor versions match, patch must be >= required
    if (minor == required.minor && patch < required.patch) return false;

    return true;
}

// Parse and satisfy version ranges
bool SemanticVersion::satisfiesRange(const std::string& range) const {
    // Trim whitespace
    std::string trimmed_range = range;
    trimmed_range.erase(0, trimmed_range.find_first_not_of(" \t"));
    trimmed_range.erase(trimmed_range.find_last_not_of(" \t") + 1);

    // Handle special range operators
    // ^1.2.3 (caret): >=1.2.3,<2.0.0 (compatible changes)
    if (trimmed_range[0] == '^') {
        SemanticVersion base = parse(trimmed_range.substr(1));
        SemanticVersion upper(base.major + 1, 0, 0);
        return *this >= base && *this < upper;
    }

    // ~1.2.3 (tilde): >=1.2.3,<1.3.0 (patch-level changes)
    if (trimmed_range[0] == '~') {
        SemanticVersion base = parse(trimmed_range.substr(1));
        SemanticVersion upper(base.major, base.minor + 1, 0);
        return *this >= base && *this < upper;
    }

    // 1.x or 1.2.x: allow any patch/minor in range
    if (trimmed_range.find('x') != std::string::npos || trimmed_range.find('X') != std::string::npos) {
        std::string normalized = trimmed_range;
        std::replace(normalized.begin(), normalized.end(), 'X', 'x');

        // Parse the x-range
        std::regex x_pattern(R"(^(\d+)\.x$)");  // e.g., "1.x"
        std::regex xx_pattern(R"(^(\d+)\.(\d+)\.x$)");  // e.g., "1.2.x"

        std::smatch matches;
        if (std::regex_match(normalized, matches, x_pattern)) {
            int base_major = std::stoi(matches[1].str());
            return major == base_major;
        } else if (std::regex_match(normalized, matches, xx_pattern)) {
            int base_major = std::stoi(matches[1].str());
            int base_minor = std::stoi(matches[2].str());
            return major == base_major && minor == base_minor;
        }
    }

    // Simple comparison operators: >=, >, <=, <, =
    // Multiple conditions separated by comma: ">=1.0.0,<2.0.0"
    std::vector<std::string> conditions;
    std::stringstream ss(trimmed_range);
    std::string condition;
    while (std::getline(ss, condition, ',')) {
        // Trim condition
        condition.erase(0, condition.find_first_not_of(" \t"));
        condition.erase(condition.find_last_not_of(" \t") + 1);
        conditions.push_back(condition);
    }

    // All conditions must be satisfied
    for (const auto& cond : conditions) {
        if (cond.size() < 2) return false;

        // Parse operator and version
        std::string op, version_str;

        if (cond.substr(0, 2) == ">=" || cond.substr(0, 2) == "<=" || cond.substr(0, 2) == "!=") {
            op = cond.substr(0, 2);
            version_str = cond.substr(2);
        } else if (cond[0] == '>' || cond[0] == '<' || cond[0] == '=') {
            op = cond.substr(0, 1);
            version_str = cond.substr(1);
        } else {
            // No operator means exact match
            op = "=";
            version_str = cond;
        }

        // Trim version string
        version_str.erase(0, version_str.find_first_not_of(" \t"));
        version_str.erase(version_str.find_last_not_of(" \t") + 1);

        try {
            SemanticVersion target = parse(version_str);

            // Check condition
            bool satisfied = false;
            if (op == ">=") satisfied = (*this >= target);
            else if (op == ">") satisfied = (*this > target);
            else if (op == "<=") satisfied = (*this <= target);
            else if (op == "<") satisfied = (*this < target);
            else if (op == "=") satisfied = (*this == target);
            else if (op == "!=") satisfied = (*this != target);

            if (!satisfied) return false;
        } catch (const VersionParseException&) {
            return false;  // Invalid version in range
        }
    }

    return true;  // All conditions satisfied
}

// Check compatibility status
Compatibility checkCompatibility(const SemanticVersion& current,
                                 const SemanticVersion& required) {
    // Major version mismatch = breaking change
    if (current.major != required.major) {
        return Compatibility::BREAKING_CHANGE;
    }

    // Minor version too old = missing features
    if (current.minor < required.minor) {
        return Compatibility::FEATURE_MISSING;
    }

    // Same major.minor, different patch
    if (current.minor == required.minor && current.patch != required.patch) {
        return Compatibility::PATCH_OUTDATED;
    }

    // Compatible
    return Compatibility::COMPATIBLE;
}

// Get human-readable compatibility message
std::string getCompatibilityMessage(Compatibility compat,
                                   const SemanticVersion& current,
                                   const SemanticVersion& required) {
    switch (compat) {
        case Compatibility::COMPATIBLE:
            return fmt::format("Version {} is compatible with {}",
                             current.toString(), required.toString());

        case Compatibility::BREAKING_CHANGE:
            return fmt::format("Breaking change: current version {} is incompatible with required {} (major version mismatch)",
                             current.toString(), required.toString());

        case Compatibility::FEATURE_MISSING:
            return fmt::format("Missing features: current version {} is older than required {} (minor version too low)",
                             current.toString(), required.toString());

        case Compatibility::PATCH_OUTDATED:
            return fmt::format("Patch outdated: current version {} differs from {} in patch level only",
                             current.toString(), required.toString());

        default:
            return "Unknown compatibility status";
    }
}

} // namespace versioning
} // namespace naab
