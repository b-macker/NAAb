#ifndef NAAB_TEMP_FILE_GUARD_H
#define NAAB_TEMP_FILE_GUARD_H

#include <filesystem>
#include <string>
#include <fmt/core.h>

namespace naab {
namespace runtime {

class TempFileGuard {
public:
    TempFileGuard(const std::filesystem::path& path) : path_(path) {
        // Silent: guard created
    }

    ~TempFileGuard() {
        if (!path_.empty() && std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
            // Silent: temp file deleted
        }
    }

    const std::filesystem::path& getPath() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace runtime
} // namespace naab

#endif // NAAB_TEMP_FILE_GUARD_H
