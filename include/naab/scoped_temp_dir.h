// NAAb ScopedTempDir — a private compile directory that is removed however
// the scope exits.
//
// The compiled-language executors (C++, Rust, Go, C#, Nim, Julia, Zig) each
// create an mkdtemp directory per block (V-RCE-004 / V-RCE-010: exclusive,
// unpredictable, mode 0700). Cleanup used to be written out per exit path, and
// it was short: C#, Nim, Julia and Zig removed the files but never the
// directory, the C++ cache-miss paths kept theirs on purpose under a comment
// claiming the cache needed it (the cache holds its own copy), and every
// executor without a try/catch leaked on an exception such as a --timeout
// firing mid-compile. One guard owns the directory instead, so a new exit
// path cannot forget it.
//
// POSIX only (mkdtemp, chmod): every executor that includes this is removed
// from the Windows build in CMakeLists.txt.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <sys/stat.h>

namespace naab {
namespace runtime {

class ScopedTempDir {
public:
    // Creates <base>/<prefix>XXXXXX with mkdtemp and restricts it to 0700.
    // On failure valid() is false and the destructor does nothing.
    ScopedTempDir(const std::filesystem::path& base, const std::string& prefix) {
        std::string tmpl = (base / (prefix + "XXXXXX")).string();
        if (char* raw = mkdtemp(tmpl.data())) {
            chmod(raw, 0700);
            path_ = raw;
        }
    }

    ~ScopedTempDir() {
        if (path_.empty()) return;
        std::error_code ec;  // a destructor must not throw; a failed removal leaves the dir
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = delete;
    ScopedTempDir& operator=(ScopedTempDir&&) = delete;

    bool valid() const { return !path_.empty(); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace runtime
} // namespace naab
