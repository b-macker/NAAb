#pragma once

// BoundedRead — symlink-rejecting, size-capped file read helper.
//
// V-RT-014 (R24): ModuleRegistry::parseModuleFile, ModuleResolver::parseModuleFile,
// and ProjectContextLoader::parseMarkdownFile/parseJsonConfig all used unbounded
// rdbuf() / istreambuf_iterator reads. An attacker can symlink a loaded file to
// /dev/zero (or similar) and trigger std::bad_alloc OOM during module loading
// or context scanning. This helper mirrors the R22 scanner pattern: lstat()
// rejects symlinks, and a 65 KB chunked read is capped at max_bytes.

#include <optional>
#include <string>
#include <cstddef>

namespace naab {

// Read `path` into a string, subject to two guards:
//   1) On POSIX, reject symlinks via lstat() + S_ISLNK (defense-in-depth: the
//      subsequent ifstream open is a second chance for TOCTOU-swapped paths,
//      but the lstat check fails fast on obvious attacks).
//   2) Read is chunked in 64 KB blocks and truncated at `max_bytes` (default
//      10 MB). Partial content is still returned — callers that want strict
//      all-or-nothing behaviour can check the returned size.
//
// Returns std::nullopt when the file cannot be opened or is a symlink.
std::optional<std::string> readFileBounded(
    const std::string& path,
    std::size_t max_bytes = 10ULL * 1024 * 1024);

} // namespace naab
