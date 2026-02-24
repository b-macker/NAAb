#pragma once

// NAAb Sandboxing and Permissions System
// Provides capability-based access control for blocks

#include <string>
#include <vector>
#include <unordered_set>
#include <memory>
#include <stdexcept>

namespace naab {
namespace security {

// Capability flags for fine-grained permissions
enum class Capability {
    // Filesystem capabilities
    FS_READ,              // Read files
    FS_WRITE,             // Write/modify files
    FS_EXECUTE,           // Execute files
    FS_DELETE,            // Delete files
    FS_CREATE_DIR,        // Create directories

    // Network capabilities
    NET_CONNECT,          // Outbound network connections
    NET_LISTEN,           // Listen on network ports
    NET_RAW,              // Raw socket access

    // System capabilities
    SYS_EXEC,             // Execute external processes
    SYS_ENV,              // Access environment variables
    SYS_TIME,             // Access system time

    // Inter-block capabilities
    BLOCK_LOAD,           // Load other blocks
    BLOCK_CALL,           // Call functions in other blocks

    // Resource capabilities
    RES_UNLIMITED_MEM,    // Bypass memory limits
    RES_UNLIMITED_CPU,    // Bypass CPU time limits

    // Special capabilities
    UNSAFE,               // Unrestricted access (use with caution)
};

// Permission level presets
enum class PermissionLevel {
    RESTRICTED,    // Minimal permissions (read-only, no network, no exec)
    STANDARD,      // Normal permissions (read/write in sandbox, no network)
    ELEVATED,      // Extended permissions (network, limited system access)
    UNRESTRICTED   // Full access (bypasses all restrictions)
};

// Sandbox configuration
struct SandboxConfig {
    // Capabilities granted to the block
    std::unordered_set<Capability> capabilities;

    // Filesystem whitelist (allowed paths)
    std::vector<std::string> allowed_read_paths;
    std::vector<std::string> allowed_write_paths;
    std::vector<std::string> allowed_exec_paths;

    // Network restrictions
    std::vector<std::string> allowed_hosts;     // Empty = all allowed
    std::vector<int> allowed_ports;             // Empty = all allowed
    bool network_enabled;

    // Resource limits
    size_t max_memory_mb;
    unsigned int max_cpu_seconds;
    size_t max_file_size_mb;

    // Execution restrictions
    bool allow_fork;
    bool allow_exec;
    std::vector<std::string> allowed_commands;  // Whitelist of executable names

    // Create config from permission level
    static SandboxConfig fromPermissionLevel(PermissionLevel level);

    // Add a capability
    void addCapability(Capability cap);

    // Check if capability is granted
    bool hasCapability(Capability cap) const;

    // Whitelist management
    void allowReadPath(const std::string& path);
    void allowWritePath(const std::string& path);
    void allowExecutePath(const std::string& path);
};

// Sandbox enforcement engine
class Sandbox {
public:
    explicit Sandbox(const SandboxConfig& config);
    ~Sandbox();

    // File access validation
    bool canRead(const std::string& path) const;
    bool canWrite(const std::string& path) const;
    bool canExecute(const std::string& path) const;
    bool canDelete(const std::string& path) const;

    // Network access validation
    bool canConnect(const std::string& host, int port) const;
    bool canListen(int port) const;

    // System operation validation
    bool canExecuteCommand(const std::string& command) const;
    bool canAccessEnv(const std::string& var_name) const;

    // Block interaction validation
    bool canLoadBlock(const std::string& block_id) const;
    bool canCallBlock(const std::string& block_id) const;

    // Get current configuration
    const SandboxConfig& getConfig() const { return config_; }

    // Audit logging
    void logViolation(const std::string& operation,
                     const std::string& resource,
                     const std::string& reason) const;

private:
    SandboxConfig config_;

    // Helper: Check if path is within allowed directory
    bool isPathAllowed(const std::string& path,
                      const std::vector<std::string>& allowed_paths) const;

    // Helper: Normalize and resolve path
    std::string normalizePath(const std::string& path) const;
};

// RAII sandbox activation
class ScopedSandbox {
public:
    explicit ScopedSandbox(const SandboxConfig& config);
    ~ScopedSandbox();

    // Get current active sandbox (thread-local)
    static Sandbox* getCurrent();

private:
    std::unique_ptr<Sandbox> sandbox_;
    Sandbox* prev_sandbox_;
};

// Global sandbox management
class SandboxManager {
public:
    static SandboxManager& instance();

    // Set default sandbox for new blocks
    void setDefaultConfig(const SandboxConfig& config);
    const SandboxConfig& getDefaultConfig() const;

    // Create sandbox config for a specific block
    SandboxConfig createConfigForBlock(const std::string& block_id,
                                      PermissionLevel level);

    // Register block permission overrides
    void registerBlockPermissions(const std::string& block_id,
                                  const SandboxConfig& config);

    // Get sandbox config for a block (uses override or default)
    SandboxConfig getConfigForBlock(const std::string& block_id);

private:
    SandboxManager();

    SandboxConfig default_config_;
    std::unordered_map<std::string, SandboxConfig> block_configs_;
};

// Exception thrown on sandbox violation
class SandboxViolationException : public std::runtime_error {
public:
    SandboxViolationException(const std::string& operation,
                             const std::string& resource,
                             const std::string& reason)
        : std::runtime_error(formatMessage(operation, resource, reason)),
          operation_(operation),
          resource_(resource),
          reason_(reason) {}

    const std::string& getOperation() const { return operation_; }
    const std::string& getResource() const { return resource_; }
    const std::string& getReason() const { return reason_; }

private:
    std::string operation_;
    std::string resource_;
    std::string reason_;

    static std::string formatMessage(const std::string& op,
                                     const std::string& res,
                                     const std::string& reason);
};

} // namespace security
} // namespace naab

