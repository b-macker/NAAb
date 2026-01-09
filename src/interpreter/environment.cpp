// Environment - Variable scoping and storage
// Using Abseil flat_hash_map for efficient lookups

#include "absl/container/flat_hash_map.h"
#include <string>
#include <memory>

namespace naab {
namespace interpreter {

class Environment {
public:
    // TODO: Implement variable storage using Abseil hashmap
private:
    absl::flat_hash_map<std::string, int> variables_;  // Stub
    std::shared_ptr<Environment> parent_;
};

} // namespace interpreter
} // namespace naab
