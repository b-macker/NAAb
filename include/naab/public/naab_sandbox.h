#pragma once
// NAAb Sandbox Configuration — Public API
// Configure security capabilities for embedded NAAb scripts.
// Stable within minor versions (0.7.x).
//
// Usage:
//   #include "naab/public/naab_sandbox.h"
//
//   naab::SandboxConfig cfg =
//       naab::SandboxConfig::fromPermissionLevel(naab::PermissionLevel::RESTRICTED);
//   cfg.allowReadPath("/path/to/project");

#include "naab/sandbox.h"

namespace naab {

// Re-export stable sandbox types into top-level naab:: namespace
using security::Capability;
using security::PermissionLevel;
using security::SandboxConfig;
using security::SandboxViolationException;

} // namespace naab
