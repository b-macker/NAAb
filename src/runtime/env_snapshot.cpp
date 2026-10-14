// The load-time environment snapshot -- see include/naab/env_snapshot.h.

#include "naab/env_snapshot.h"

#include <cctype>
#include <mutex>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <crt_externs.h>
#  define NAAB_SNAPSHOT_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#  define NAAB_SNAPSHOT_ENVIRON environ
#endif

namespace naab {

namespace {

std::once_flag g_once;
std::vector<std::string> g_env;   // written once inside call_once, then read-only

std::vector<std::string> readProcessEnvironment() {
    std::vector<std::string> out;
#ifndef _WIN32
    for (char** e = NAAB_SNAPSHOT_ENVIRON; e && *e; ++e) out.emplace_back(*e);
#else
    LPCH block = ::GetEnvironmentStringsA();
    if (block) {
        for (LPCH p = block; *p;) {
            std::string entry(p);
            p += entry.size() + 1;
            // "=C:=C:\dir" entries are per-drive cwd bookkeeping, not variables.
            if (!entry.empty() && entry[0] != '=') out.push_back(std::move(entry));
        }
        ::FreeEnvironmentStringsA(block);
    }
#endif
    return out;
}

#ifdef _WIN32
bool asciiEqualsIgnoreCase(const std::string& a, size_t a_len, const std::string& b) {
    if (a_len != b.size()) return false;
    for (size_t i = 0; i < a_len; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
#endif

} // namespace

void captureEnvironmentSnapshotOnce() {
    std::call_once(g_once, [] { g_env = readProcessEnvironment(); });
}

const std::vector<std::string>& environmentSnapshot() {
    captureEnvironmentSnapshotOnce();
    return g_env;
}

std::string snapshotEnv(const std::string& key) {
    const auto& env = environmentSnapshot();
    for (const auto& kv : env)
        if (kv.size() > key.size() && kv[key.size()] == '=' && kv.compare(0, key.size(), key) == 0)
            return kv.substr(key.size() + 1);
#ifdef _WIN32
    for (const auto& kv : env)
        if (kv.size() > key.size() && kv[key.size()] == '=' && asciiEqualsIgnoreCase(kv, key.size(), key))
            return kv.substr(key.size() + 1);
#endif
    return "";
}

} // namespace naab
