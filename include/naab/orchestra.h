// orchestra.h — Multi-agent coordination patterns
// Provides high-level orchestration functions for agent workflows

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "naab/stdlib.h"
#include "naab/naab_val.h"

namespace naab {
namespace stdlib {

// ============================================================================
// Orchestra Module — Multi-Agent Patterns
// ============================================================================

class OrchestraModule : public Module {
public:
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Implementation functions (exported for testing)
interpreter::NaabVal orchestraSequentialRefinement(std::vector<interpreter::NaabVal>& args);
interpreter::NaabVal orchestraConsensusVote(std::vector<interpreter::NaabVal>& args);
interpreter::NaabVal orchestraEnforceConvergence(std::vector<interpreter::NaabVal>& args);

}  // namespace stdlib
}  // namespace naab
