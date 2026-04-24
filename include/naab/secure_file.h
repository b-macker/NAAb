#pragma once

// SecureFile — symlink-resistant file write helper.
//
// V-SC-005 (R24): std::ofstream follows symlinks, allowing an attacker who
// pre-creates a symlink at the target path (e.g. naab.lock -> ~/.ssh/id_rsa)
// to trick NAAb into overwriting sensitive files.
//
// writeFileSecure() uses open() with O_NOFOLLOW on POSIX so that attempting to
// open a symlinked path fails with ELOOP. On Windows, it uses CreateFileA with
// reparse-point rejection.

#include <string>

namespace naab {
namespace security {

// Atomically write `content` to `path`, refusing to follow symlinks.
// Returns true on success, false on any error (including "path is a symlink").
// If the file already exists as a regular file, it is truncated and overwritten.
// mode: POSIX file permission bits (default 0644). Ignored on Windows.
bool writeFileSecure(const std::string& path, const std::string& content,
                     int mode = 0644);

} // namespace security
} // namespace naab
