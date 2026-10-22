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
// GenericSubprocessExecutor (python where there is no embedded Python, php,
// typescript) writes its script into one of these too. It used to pick the
// first free name in the shared temp directory (naab_temp_python.py, then
// _0, _1, ...): concurrent blocks -- VM async fns, or two processes -- took the
// same name, and one block's cleanup deleted the script another was about to
// run ("python3: can't open file ... naab_temp_python_4.py", 6 of 10 runs of
// 16 concurrent async fns). It was also the predictable shared name V-RCE-004
// removed from the compiled executors, and Python puts the script's directory
// first on sys.path, so a module planted in the shared directory was
// importable by the block. That executor also builds on Windows, so this
// guard has a Windows branch: a random name created with create_directory(),
// which fails when the name exists, so the directory is still the caller's
// alone. The temp directory there is per user.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <random>
#else
#include <sys/stat.h>
#endif

namespace naab {
namespace runtime {

class ScopedTempDir {
public:
    // Creates <base>/<prefix>XXXXXX with mkdtemp and restricts it to 0700
    // (POSIX), or a new randomly named directory (Windows).
    // On failure valid() is false and the destructor does nothing.
    ScopedTempDir(const std::filesystem::path& base, const std::string& prefix) {
#ifdef _WIN32
        // No mkdtemp: try random names until create_directory() makes a new
        // one. It returns false for a name that already exists, so a
        // directory someone else made is never adopted.
        std::random_device rd;
        static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        for (int attempt = 0; attempt < 100 && path_.empty(); ++attempt) {
            std::string name = prefix;
            for (int i = 0; i < 12; ++i) name += kChars[rd() % 36];
            std::error_code ec;
            std::filesystem::path p = base / name;
            if (std::filesystem::create_directory(p, ec) && !ec) path_ = p;
        }
#else
        std::string tmpl = (base / (prefix + "XXXXXX")).string();
        if (char* raw = mkdtemp(tmpl.data())) {
            chmod(raw, 0700);
            path_ = raw;
        }
#endif
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
