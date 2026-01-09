// Symbol Table - Variable and function scope management
// Using Abseil flat_hash_map blocks

#include "absl/container/flat_hash_map.h"
#include <string>

namespace naab {
namespace semantic {

class SymbolTable {
public:
    // TODO: Implement symbol table using absl::flat_hash_map
private:
    absl::flat_hash_map<std::string, int> symbols_;  // Stub
};

} // namespace semantic
} // namespace naab
