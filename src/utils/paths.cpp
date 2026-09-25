#include "naab/paths.h"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace naab {
namespace paths {

std::string home() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
    if (h && h[0]) return h;
    const char* d = std::getenv("HOMEDRIVE");
    const char* p = std::getenv("HOMEPATH");
    if (d && p && d[0] && p[0]) return std::string(d) + std::string(p);
#else
    const char* h = std::getenv("HOME");
    if (h && h[0]) return h;
#endif
    return ".";
}

std::string temp_dir() {
    const char* t = std::getenv("TMPDIR");
    if (t && t[0]) return t;
    t = std::getenv("TMP");
    if (t && t[0]) return t;
    t = std::getenv("TEMP");
    if (t && t[0]) return t;
    // Try C++17 filesystem
    std::error_code ec;
    auto p = std::filesystem::temp_directory_path(ec);
    if (!ec && !p.empty()) return p.string();
#ifdef _WIN32
    return "C:\\Temp";
#else
    return "/tmp";
#endif
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
    // Python was not found at configure time. This used to return a hardcoded
    // python3.12 path, which is wrong on every machine with another minor
    // version -- the C++ executor then compiled with a nonexistent -I and any
    // block including <Python.h> failed. Scan the usual roots for the highest
    // python3.N that actually holds Python.h; no subprocess, filesystem only.
    std::vector<std::string> roots;
    const char* prefix = std::getenv("PREFIX");
    if (prefix && prefix[0]) roots.push_back(std::string(prefix) + "/include");
    roots.push_back("/usr/local/include");
    roots.push_back("/usr/include");
    std::error_code ec;
    for (const auto& root : roots) {
        std::string best;
        long best_minor = -1;
        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
            std::string name = e.path().filename().string();
            if (name.rfind("python3.", 0) != 0) continue;
            const char* digits = name.c_str() + 8;
            char* endp = nullptr;
            long minor = std::strtol(digits, &endp, 10);
            if (endp == digits) continue;
            if (!std::filesystem::exists(e.path() / "Python.h", ec)) continue;
            if (minor > best_minor) { best_minor = minor; best = e.path().string(); }
        }
        if (!best.empty()) return best;
    }
    // Nothing found: keep the historical guess so the failure mode is unchanged.
    if (prefix && prefix[0]) {
        return std::string(prefix) + "/include/python3.12";
    }
    return "/usr/include/python3.12";
#endif
}

} // namespace paths
} // namespace naab
