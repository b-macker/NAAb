// NAAb Embedding API — Context Implementation (Phase 8.1)
// Implements naab::Context via Pimpl pattern.
// Keeps all internal headers (Python.h, AST, interpreter internals) out of the
// public naab_interpreter.h header.

#define NAAB_BUILDING_DLL 1
#include "naab/public/naab_interpreter.h"
#include "naab/public/naab_sandbox.h"
#include "naab/interpreter.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/sandbox.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace naab {

// ============================================================================
// Context::Impl — holds all internal state
// ============================================================================

struct Context::Impl {
    std::unique_ptr<interpreter::Interpreter> interp;
    security::SandboxConfig sandbox_config;
    bool governance_enabled = true;

    explicit Impl(const std::string& sandbox_level) {
        static const std::unordered_map<std::string, security::PermissionLevel> levels = {
            {"restricted",   security::PermissionLevel::RESTRICTED},
            {"standard",     security::PermissionLevel::STANDARD},
            {"elevated",     security::PermissionLevel::ELEVATED},
            {"unrestricted", security::PermissionLevel::UNRESTRICTED},
        };
        auto it = levels.find(sandbox_level);
        if (it == levels.end()) {
            throw std::invalid_argument(
                "Unknown sandbox level: \"" + sandbox_level + "\"\n"
                "Valid levels: restricted, standard, elevated, unrestricted"
            );
        }
        sandbox_config = security::SandboxConfig::fromPermissionLevel(it->second);
        interp = std::make_unique<interpreter::Interpreter>();
    }
};

// ============================================================================
// Context — public API
// ============================================================================

Context::Context(const std::string& sandbox_level)
    : impl_(std::make_unique<Impl>(sandbox_level)) {}

Context::~Context() = default;
Context::Context(Context&&) noexcept = default;
Context& Context::operator=(Context&&) noexcept = default;

Val Context::eval(const std::string& source, const std::string& filename) {
    if (!impl_->governance_enabled) {
        impl_->interp->disableGovernance();
    }

    // Activate sandbox for the duration of this eval
    security::ScopedSandbox guard(impl_->sandbox_config);

    // Lex
    naab::lexer::Lexer lexer(source);
    auto tokens = lexer.tokenize();

    // Parse
    naab::parser::Parser parser(tokens);
    parser.setSource(source, filename);
    auto program = parser.parseProgram();

    // Execute (tree-walk interpreter)
    impl_->interp->setSourceCode(source, filename);
    impl_->interp->execute(*program);

    return impl_->interp->getLastResult();
}

Val Context::evalFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("Context::evalFile: cannot open: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return eval(ss.str(), path);
}

void Context::setVerbose(bool v) {
    impl_->interp->setVerboseMode(v);
}

void Context::setGovernanceEnabled(bool enabled) {
    impl_->governance_enabled = enabled;
    if (!enabled) {
        impl_->interp->disableGovernance();
    }
}

void Context::setSandboxConfig(const SandboxConfig& cfg) {
    impl_->sandbox_config = cfg;
}

} // namespace naab
