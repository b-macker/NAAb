#pragma once

// `process.run("python3", ["-c", CODE])` is a polyglot block by another name:
// the stdlib checks CODE as that language (src/stdlib/process_impl.cpp). The
// one recogniser, shared with `naab-gov manifest infer` so a drafted manifest
// names the language the run will check.

#include <string>
#include <vector>

namespace naab {
namespace stdlib {

// True when `cmd argv...` runs inline interpreter code; then `lang` is the
// governance language name and `code` the code.
bool inlineInterpreterCode(const std::string& cmd,
                           const std::vector<std::string>& argv,
                           std::string& lang, std::string& code);

}  // namespace stdlib
}  // namespace naab
