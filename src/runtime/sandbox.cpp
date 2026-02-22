// NAAb Sandboxing Implementation
// Capability-based access control and resource isolation

#include "naab/sandbox.h"
#include "naab/paths.h"
#include "naab/audit_logger.h"
#include "naab/logger.h"
#include <fmt/core.h>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

namespace naab {
namespace security {

// Thread-local current sandbox
thread_local Sandbox* current_sandbox = nullptr;

// ============================================================================
// SandboxConfig Implementation
// ============================================================================

SandboxConfig SandboxConfig::fromPermissionLevel(PermissionLevel level) {
    SandboxConfig config;

    switch (level) {
        case PermissionLevel::RESTRICTED:
            // Read-only, no network, no execution
            config.capabilities.insert(Capability::FS_READ);
            config.network_enabled = false;
            config.allow_fork = false;
            config.allow_exec = false;
            config.max_memory_mb = 128;
            config.max_cpu_seconds = 10;
            config.max_file_size_mb = 10;
            break;

        case PermissionLevel::STANDARD:
            // Read/write in sandbox, no network, limited execution
            config.capabilities.insert(Capability::FS_READ);
            config.capabilities.insert(Capability::FS_WRITE);
            config.capabilities.insert(Capability::FS_CREATE_DIR);
            config.capabilities.insert(Capability::BLOCK_LOAD);
            config.capabilities.insert(Capability::BLOCK_CALL);
            config.capabilities.insert(Capability::SYS_ENV);
            config.capabilities.insert(Capability::SYS_TIME);
            config.network_enabled = false;
            config.allow_fork = false;
            config.allow_exec = false;
            config.max_memory_mb = 512;
            config.max_cpu_seconds = 30;
            config.max_file_size_mb = 100;

            // Allow read/write in temp and user dirs
            config.allowed_read_paths.push_back("/tmp");
            config.allowed_read_paths.push_back(naab::paths::home());
            config.allowed_write_paths.push_back("/tmp");
            break;

        case PermissionLevel::ELEVATED:
            // Network access, system interaction, controlled execution
            config.capabilities.insert(Capability::FS_READ);
            config.capabilities.insert(Capability::FS_WRITE);
            config.capabilities.insert(Capability::FS_CREATE_DIR);
            config.capabilities.insert(Capability::NET_CONNECT);
            config.capabilities.insert(Capability::BLOCK_LOAD);
            config.capabilities.insert(Capability::BLOCK_CALL);
            config.capabilities.insert(Capability::SYS_ENV);
            config.capabilities.insert(Capability::SYS_TIME);
            config.capabilities.insert(Capability::SYS_EXEC);
            config.network_enabled = true;
            config.allow_fork = true;
            config.allow_exec = true;
            config.max_memory_mb = 1024;
            config.max_cpu_seconds = 60;
            config.max_file_size_mb = 1000;
            break;

        case PermissionLevel::UNRESTRICTED:
            // Full access (use with caution!)
            config.capabilities.insert(Capability::UNSAFE);
            config.network_enabled = true;
            config.allow_fork = true;
            config.allow_exec = true;
            config.max_memory_mb = 0;  // No limit
            config.max_cpu_seconds = 0;  // No limit
            config.max_file_size_mb = 0;  // No limit
            break;
    }

    return config;
}

void SandboxConfig::addCapability(Capability cap) {
    capabilities.insert(cap);
}

bool SandboxConfig::hasCapability(Capability cap) const {
    // UNSAFE grants all capabilities
    if (capabilities.count(Capability::UNSAFE)) {
        return true;
    }
    return capabilities.count(cap) > 0;
}

void SandboxConfig::allowReadPath(const std::string& path) {
    allowed_read_paths.push_back(path);
}

void SandboxConfig::allowWritePath(const std::string& path) {
    allowed_write_paths.push_back(path);
}

void SandboxConfig::allowExecutePath(const std::string& path) {
    allowed_exec_paths.push_back(path);
}

// ============================================================================
// Sandbox Implementation
// ============================================================================

Sandbox::Sandbox(const SandboxConfig& config)
    : config_(config) {
    LOG_DEBUG("[SANDBOX] Initialized with {} capabilities\n", config_.capabilities.size());
}

Sandbox::~Sandbox() {
    // Cleanup
}

std::string Sandbox::normalizePath(const std::string& path) const {
    try {
        // Use realpath to resolve symlinks and normalize
        char resolved[PATH_MAX];
        if (realpath(path.c_str(), resolved)) {
            return std::string(resolved);
        }

        // If realpath fails (file doesn't exist), do basic normalization
        std::string normalized = path;

        // Remove trailing slashes
        while (normalized.size() > 1 && normalized.back() == '/') {
            normalized.pop_back();
        }

        return normalized;
    } catch (...) {
        return path;  // Return original on error
    }
}

bool Sandbox::isPathAllowed(const std::string& path,
                            const std::vector<std::string>& allowed_paths) const {
    // Normalize the path
    std::string normalized = normalizePath(path);

    // Check if path starts with any allowed path
    for (const auto& allowed : allowed_paths) {
        std::string allowed_norm = normalizePath(allowed);

        // Check if path is within allowed directory
        if (normalized.find(allowed_norm) == 0) {
            // Ensure it's actually a subdirectory (not just prefix match)
            if (normalized.size() == allowed_norm.size() ||
                normalized[allowed_norm.size()] == '/') {
                return true;
            }
        }
    }

    return false;
}

bool Sandbox::canRead(const std::string& path) const {
    // Check FS_READ capability
    if (!config_.hasCapability(Capability::FS_READ)) {
        return false;
    }

    // If no whitelist, allow all
    if (config_.allowed_read_paths.empty()) {
        return true;
    }

    // Check whitelist
    return isPathAllowed(path, config_.allowed_read_paths);
}

bool Sandbox::canWrite(const std::string& path) const {
    // Check FS_WRITE capability
    if (!config_.hasCapability(Capability::FS_WRITE)) {
        return false;
    }

    // If no whitelist, allow all
    if (config_.allowed_write_paths.empty()) {
        return true;
    }

    // Check whitelist
    return isPathAllowed(path, config_.allowed_write_paths);
}

bool Sandbox::canExecute(const std::string& path) const {
    // Check FS_EXECUTE capability
    if (!config_.hasCapability(Capability::FS_EXECUTE)) {
        return false;
    }

    // If no whitelist, allow all
    if (config_.allowed_exec_paths.empty()) {
        return true;
    }

    // Check whitelist
    return isPathAllowed(path, config_.allowed_exec_paths);
}

bool Sandbox::canDelete(const std::string& path) const {
    // Deletion requires FS_DELETE capability
    if (!config_.hasCapability(Capability::FS_DELETE)) {
        return false;
    }

    // Also check write permissions (delete is a write operation)
    return canWrite(path);
}

bool Sandbox::canConnect(const std::string& host, int port) const {
    // Check if network is enabled
    if (!config_.network_enabled) {
        return false;
    }

    // Check NET_CONNECT capability
    if (!config_.hasCapability(Capability::NET_CONNECT)) {
        return false;
    }

    // Check host whitelist (if specified)
    if (!config_.allowed_hosts.empty()) {
        bool host_allowed = false;
        for (const auto& allowed_host : config_.allowed_hosts) {
            if (host == allowed_host || host.find(allowed_host) != std::string::npos) {
                host_allowed = true;
                break;
            }
        }
        if (!host_allowed) {
            return false;
        }
    }

    // Check port whitelist (if specified)
    if (!config_.allowed_ports.empty()) {
        bool port_allowed = std::find(config_.allowed_ports.begin(),
                                     config_.allowed_ports.end(),
                                     port) != config_.allowed_ports.end();
        if (!port_allowed) {
            return false;
        }
    }

    return true;
}

bool Sandbox::canListen(int port) const {
    // Check if network is enabled
    if (!config_.network_enabled) {
        return false;
    }

    // Check NET_LISTEN capability
    if (!config_.hasCapability(Capability::NET_LISTEN)) {
        return false;
    }

    // Check port whitelist (if specified)
    if (!config_.allowed_ports.empty()) {
        return std::find(config_.allowed_ports.begin(),
                        config_.allowed_ports.end(),
                        port) != config_.allowed_ports.end();
    }

    return true;
}

bool Sandbox::canExecuteCommand(const std::string& command) const {
    // Check if exec is allowed
    if (!config_.allow_exec) {
        return false;
    }

    // Check SYS_EXEC capability
    if (!config_.hasCapability(Capability::SYS_EXEC)) {
        return false;
    }

    // Extract command name (first token)
    std::string cmd_name = command;
    size_t space_pos = command.find(' ');
    if (space_pos != std::string::npos) {
        cmd_name = command.substr(0, space_pos);
    }

    // Check command whitelist (if specified)
    if (!config_.allowed_commands.empty()) {
        return std::find(config_.allowed_commands.begin(),
                        config_.allowed_commands.end(),
                        cmd_name) != config_.allowed_commands.end();
    }

    return true;
}

bool Sandbox::canAccessEnv(const std::string& var_name) const {
    (void)var_name;  // Reserved for future per-variable access control
    // Check SYS_ENV capability
    return config_.hasCapability(Capability::SYS_ENV);
}

bool Sandbox::canLoadBlock(const std::string& block_id) const {
    (void)block_id;  // Reserved for future per-block access control
    // Check BLOCK_LOAD capability
    return config_.hasCapability(Capability::BLOCK_LOAD);
}

bool Sandbox::canCallBlock(const std::string& block_id) const {
    (void)block_id;  // Reserved for future per-block access control
    // Check BLOCK_CALL capability
    return config_.hasCapability(Capability::BLOCK_CALL);
}

void Sandbox::logViolation(const std::string& operation,
                          const std::string& resource,
                          const std::string& reason) const {
    AuditLogger::logSecurityViolation(
        fmt::format("Sandbox violation: {} on '{}' - {}", operation, resource, reason)
    );

    fmt::print(stderr, "[SANDBOX VIOLATION] {} on '{}': {}\n",
               operation, resource, reason);
}

// ============================================================================
// ScopedSandbox Implementation
// ============================================================================

ScopedSandbox::ScopedSandbox(const SandboxConfig& config)
    : sandbox_(std::make_unique<Sandbox>(config)),
      prev_sandbox_(current_sandbox) {
    current_sandbox = sandbox_.get();
}

ScopedSandbox::~ScopedSandbox() {
    current_sandbox = prev_sandbox_;
}

Sandbox* ScopedSandbox::getCurrent() {
    return current_sandbox;
}

// ============================================================================
// SandboxManager Implementation
// ============================================================================

SandboxManager& SandboxManager::instance() {
    static SandboxManager mgr;
    return mgr;
}

SandboxManager::SandboxManager() {
    // Default to STANDARD permissions
    default_config_ = SandboxConfig::fromPermissionLevel(PermissionLevel::STANDARD);
}

void SandboxManager::setDefaultConfig(const SandboxConfig& config) {
    default_config_ = config;
}

const SandboxConfig& SandboxManager::getDefaultConfig() const {
    return default_config_;
}

SandboxConfig SandboxManager::createConfigForBlock(const std::string& block_id,
                                                   PermissionLevel level) {
    auto config = SandboxConfig::fromPermissionLevel(level);

    // Add block-specific sandbox directory
    std::string home = naab::paths::home();
    std::string block_sandbox = home + "/.naab/sandbox/" + block_id;

    config.allowReadPath(block_sandbox);
    config.allowWritePath(block_sandbox);

    return config;
}

void SandboxManager::registerBlockPermissions(const std::string& block_id,
                                             const SandboxConfig& config) {
    block_configs_[block_id] = config;
    fmt::print("[SANDBOX] Registered custom permissions for block: {}\n", block_id);
}

SandboxConfig SandboxManager::getConfigForBlock(const std::string& block_id) {
    // Check if block has custom config
    auto it = block_configs_.find(block_id);
    if (it != block_configs_.end()) {
        return it->second;
    }

    // Use default config
    return default_config_;
}

// ============================================================================
// SandboxViolationException Implementation
// ============================================================================

std::string SandboxViolationException::formatMessage(const std::string& op,
                                                    const std::string& res,
                                                    const std::string& reason) {
    return fmt::format("Sandbox violation: {} on '{}' - {}", op, res, reason);
}

} // namespace security
} // namespace naab
