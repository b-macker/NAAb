#pragma once
// NAAb Interpreter — Public Embedding Facade
// Stable C++ API for embedding NAAb in a host application.
// Stable within minor versions (0.7.x).
//
// This header intentionally does NOT include naab/interpreter.h (which pulls in
// <Python.h>, internal AST types, and ~600 lines of internals). It exposes only
// the stable embedding surface via the Pimpl pattern.
//
// NOTE: Context::Impl is declared but unimplemented pending Phase 8.1 (libnaab).
//       This header establishes the stable API shape for embedding consumers.
//       Linking against Context requires the libnaab shared library (Sprint 7+).

#include "naab/public/naab_val.h"
#include "naab/public/naab_sandbox.h"
#include <memory>
#include <string>

// API visibility macro
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(NAAB_BUILDING_DLL)
#    define NAAB_API __declspec(dllexport)
#  elif defined(NAAB_USING_DLL)
#    define NAAB_API __declspec(dllimport)
#  else
#    define NAAB_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define NAAB_API __attribute__((visibility("default")))
#else
#  define NAAB_API
#endif

namespace naab {

// Embedded interpreter context.
// Each Context is fully independent — separate environment, no shared state.
//
// Thread safety: single-threaded. Do not share a Context across threads.
// Create one Context per thread if concurrent execution is needed.
//
// Example:
//   naab::Context ctx("restricted");
//   naab::Val result = ctx.eval("main { 1 + 2 }");
class NAAB_API Context {
public:
    // Create a context with the given sandbox level.
    // Valid levels: "restricted", "standard", "elevated", "unrestricted"
    // Throws std::invalid_argument for unknown levels.
    explicit Context(const std::string& sandbox_level = "standard");
    ~Context();

    // Contexts are movable but not copyable.
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) noexcept;
    Context& operator=(Context&&) noexcept;

    // Execute NAAb source code from a string.
    // Returns the result of the last expression; null Val for statement-only programs.
    // Throws std::runtime_error on syntax or runtime errors.
    // Throws SandboxViolationException on sandbox violations.
    Val eval(const std::string& source,
             const std::string& filename = "<embedded>");

    // Execute a NAAb source file.
    // Throws std::runtime_error on syntax, runtime, or I/O errors.
    Val evalFile(const std::string& path);

    // Enable verbose diagnostic output to stderr (AST dumps, VM traces).
    void setVerbose(bool v);

    // Enable or disable the governance engine.
    // When enabled, govern.json is discovered from the script's directory upward.
    // Default: enabled.
    void setGovernanceEnabled(bool enabled);

    // Replace the sandbox configuration.
    // Must be called before the first eval(). Calling after eval() has no effect.
    void setSandboxConfig(const SandboxConfig& config);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace naab
