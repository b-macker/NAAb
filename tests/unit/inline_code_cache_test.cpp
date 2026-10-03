// InlineCodeCache unit tests: the on-disk cache under ~/.naab/cache is shared
// by every naab-lang process a user runs, so its files must never be
// observable half-written.
//
// storeBinary() installed the binary with copy_file(overwrite_existing) and
// saveMetadata() wrote metadata.txt through an ofstream on the final name.
// Both truncate first and then fill, so for the length of the write a reader
// in another process -- one running the cached binary, or one starting up and
// loading metadata.txt -- saw a partial file. cache_mutex_ is in-process only.
//
// Each test forks writer processes that rewrite the file in a loop while the
// parent reads it, and requires every read to be absent or complete. The
// "observed" counters are the positive control: a reader that never saw the
// file at all proves nothing.

#ifndef _WIN32

#include <gtest/gtest.h>
#include "naab/inline_code_cache.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using naab::runtime::InlineCodeCache;

namespace {

// Points HOME (where the cache lives) at a fresh directory for one test.
class TempHome {
public:
    TempHome() {
        std::string tmpl = (fs::temp_directory_path() / "naab_icc_test_XXXXXX").string();
        dir_ = mkdtemp(tmpl.data());
        const char* old = std::getenv("HOME");
        had_old_ = old != nullptr;
        if (had_old_) old_ = old;
        setenv("HOME", dir_.c_str(), 1);
    }
    ~TempHome() {
        if (had_old_) setenv("HOME", old_.c_str(), 1); else unsetenv("HOME");
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& dir() const { return dir_; }

private:
    fs::path dir_;
    std::string old_;
    bool had_old_ = false;
};

// Forked writer processes. alive() reaps each as it exits and keeps its
// status, so failures() still sees it afterwards.
class Writers {
public:
    void add(pid_t pid) { pids_.push_back(pid); done_.push_back(false); status_.push_back(0); }
    bool alive() {
        bool any = false;
        for (size_t i = 0; i < pids_.size(); i++) {
            if (done_[i]) continue;
            if (waitpid(pids_[i], &status_[i], WNOHANG) == pids_[i]) done_[i] = true;
            else any = true;
        }
        return any;
    }
    int failures() {
        int n = 0;
        for (size_t i = 0; i < pids_.size(); i++) {
            if (!done_[i] && waitpid(pids_[i], &status_[i], 0) == pids_[i]) done_[i] = true;
            if (!done_[i] || !WIFEXITED(status_[i]) || WEXITSTATUS(status_[i]) != 0) n++;
        }
        return n;
    }

private:
    std::vector<pid_t> pids_;
    std::vector<bool> done_;
    std::vector<int> status_;
};

// A metadata.txt line is "language:hash|binary|source|count|epoch\n".
bool metadataComplete(const std::string& text, size_t expected_lines) {
    if (text.empty() || text.back() != '\n') return false;
    std::istringstream in(text);
    std::string line;
    size_t lines = 0;
    while (std::getline(in, line)) {
        size_t bars = 0;
        for (char c : line) bars += (c == '|');
        if (bars != 4) return false;
        lines++;
    }
    return lines == expected_lines;
}

} // namespace

TEST(InlineCodeCacheAtomicity, CachedBinaryIsNeverObservedPartial) {
    TempHome home;
    const fs::path src = home.dir() / "compiled.bin";
    const size_t kSize = 16u << 20;
    {
        std::ofstream out(src, std::ios::binary);
        std::string chunk(1u << 20, 'x');
        for (size_t i = 0; i < kSize / chunk.size(); i++) out << chunk;
    }
    ASSERT_EQ(fs::file_size(src), kSize);

    const std::string code = "int main() { return 0; }";
    std::string cached;
    {
        InlineCodeCache probe;
        cached = probe.getBinaryPath("cpp", probe.hashCode(code));
    }

    Writers writers;
    for (int k = 0; k < 2; k++) {
        pid_t pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            {
                InlineCodeCache cache;
                for (int i = 0; i < 25; i++) {
                    cache.storeBinary("cpp", code, src.string(), "/nonexistent-source");
                }
            }
            _exit(0);
        }
        writers.add(pid);
    }

    size_t observed = 0, partial = 0;
    while (writers.alive()) {
        int fd = open(cached.c_str(), O_RDONLY);
        if (fd < 0) continue;
        struct stat st {};
        if (fstat(fd, &st) == 0) {
            observed++;
            if (static_cast<size_t>(st.st_size) != kSize) partial++;
        }
        close(fd);
    }
    EXPECT_EQ(writers.failures(), 0);

    EXPECT_GT(observed, 0u) << "reader never saw the cached binary -- the test measured nothing";
    EXPECT_EQ(partial, 0u) << partial << " of " << observed
                           << " reads saw the cached binary partially written";
    EXPECT_EQ(fs::file_size(cached), kSize);
}

TEST(InlineCodeCacheAtomicity, MetadataIsNeverObservedPartial) {
    TempHome home;
    const fs::path src = home.dir() / "tiny.bin";
    { std::ofstream(src, std::ios::binary) << "bin"; }

    const size_t kEntries = 400;
    const std::string metadata = (home.dir() / ".naab/cache/metadata.txt").string();

    Writers writers;
    for (int k = 0; k < 2; k++) {
        pid_t pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            {
                InlineCodeCache cache;
                for (size_t i = 0; i < kEntries; i++) {
                    cache.storeBinary("cpp", "block " + std::to_string(i), src.string(),
                                      "/nonexistent-source");
                }
                for (int i = 0; i < 150; i++) cache.saveMetadata();
            }
            _exit(0);
        }
        writers.add(pid);
    }

    size_t observed = 0, partial = 0;
    while (writers.alive()) {
        std::ifstream in(metadata, std::ios::binary);
        if (!in) continue;
        std::stringstream buf;
        buf << in.rdbuf();
        observed++;
        if (!metadataComplete(buf.str(), kEntries)) partial++;
    }
    EXPECT_EQ(writers.failures(), 0);

    EXPECT_GT(observed, 0u) << "reader never saw metadata.txt -- the test measured nothing";
    EXPECT_EQ(partial, 0u) << partial << " of " << observed
                           << " reads saw metadata.txt partially written";

    // The file a later process loads is whole.
    std::ifstream in(metadata, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    EXPECT_TRUE(metadataComplete(buf.str(), kEntries));
}

#endif  // _WIN32
