#include "naab/paths.h"
#include <cstdlib>
#include <filesystem>

namespace naab {
namespace paths {

std::string home() {
    const char* h = std::getenv("HOME");
    if (h && h[0]) return h;
    return ".";
}

std::string temp_dir() {
    const char* t = std::getenv("TMPDIR");
    if (t && t[0]) return t;
    t = std::getenv("TMP");
    if (t && t[0]) return t;
    t = std::getenv("TEMP");
    if (t && t[0]) return t;
    // Try filesystem
    std::error_code ec;
    auto p = std::filesystem::temp_directory_path(ec);
    if (!ec && !p.empty()) return p.string();
    return "/tmp";
}

std::string history_file() {
    return home() + "/.naab_history";
}

std::string cache_dir() {
    return home() + "/.naab/cache";
}

std::string cpp_cache_dir() {
    return home() + "/.naab_cpp_cache";
}

std::string include_dir() {
#ifdef NAAB_INCLUDE_DIR
    return NAAB_INCLUDE_DIR;
#else
    return "include";
#endif
}

std::string python_include_dir() {
#ifdef NAAB_PYTHON_INCLUDE_DIR
    return NAAB_PYTHON_INCLUDE_DIR;
#else
    // Fallback: try common locations
    const char* prefix = std::getenv("PREFIX");
    if (prefix && prefix[0]) {
        return std::string(prefix) + "/include/python3.12";
    }
    return "/usr/include/python3.12";
#endif
}

} // namespace paths
} // namespace naab
