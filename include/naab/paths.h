#pragma once

#include <string>

namespace naab {
namespace paths {

// Returns $HOME, with fallback to "."
std::string home();

// Returns temp directory: $TMPDIR, then /tmp, then std::filesystem::temp_directory_path()
std::string temp_dir();

// Returns $HOME/.naab_history
std::string history_file();

// Returns $HOME/.naab/cache
std::string cache_dir();

// Returns $HOME/.naab_cpp_cache
std::string cpp_cache_dir();

// Returns the NAAb include directory (compile-time or relative fallback)
std::string include_dir();

// Returns the Python include directory (compile-time or pkg-config fallback)
std::string python_include_dir();

} // namespace paths
} // namespace naab
