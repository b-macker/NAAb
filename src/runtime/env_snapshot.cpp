// The load-time environment snapshot -- see include/naab/env_snapshot.h.

#include "naab/env_snapshot.h"

#include <cctype>
#include <mutex>
#include <sstream>

#ifndef _WIN32
#  include <unistd.h>
#endif

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

std::vector<std::string> operatorEnvironment(bool keep_naab_keys) {
    std::vector<std::string> out;
    for (const auto& kv : environmentSnapshot()) {
        if (!keep_naab_keys &&
            (kv.rfind("NAAB_GOVERN_KEY=", 0) == 0 || kv.rfind("NAAB_LOCK_KEY=", 0) == 0 ||
             kv.rfind("NAAB_SIGNING_KEY=", 0) == 0))
            continue;
        out.push_back(kv);
    }
    return out;
}

std::string findOnSnapshotPath(const std::string& name) {
    if (name.empty()) return "";
#ifndef _WIN32
    if (name.find('/') != std::string::npos) return "";
    std::istringstream ps(snapshotEnv("PATH"));
    std::string dir;
    while (std::getline(ps, dir, ':')) {
        if (dir.empty()) continue;
        const std::string cand = dir + "/" + name;
        if (::access(cand.c_str(), X_OK) == 0) return cand;
    }
    return "";
#else
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return "";
    const std::string path = snapshotEnv("PATH");
    if (path.empty()) return "";
    char buf[MAX_PATH];
    const DWORD n = ::SearchPathA(path.c_str(), name.c_str(), ".exe", MAX_PATH, buf, nullptr);
    if (n == 0 || n >= MAX_PATH) return "";
    return std::string(buf, n);
#endif
}

} // namespace naab
