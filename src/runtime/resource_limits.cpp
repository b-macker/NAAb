#include "naab/resource_limits.h"
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <sys/resource.h>
#include <stdexcept>

namespace naab {
namespace security {

// Static member initialization
bool ResourceLimiter::initialized_ = false;
bool ResourceLimiter::timeout_triggered_ = false;

void ResourceLimiter::installSignalHandlers() {
    if (initialized_) {
        return;
    }

    // Install SIGALRM handler for execution timeout
    struct sigaction sa_alarm;
    std::memset(&sa_alarm, 0, sizeof(sa_alarm));
    sa_alarm.sa_handler = handleAlarm;
    sa_alarm.sa_flags = SA_RESTART;  // Restart interrupted system calls
    sigemptyset(&sa_alarm.sa_mask);

    if (sigaction(SIGALRM, &sa_alarm, nullptr) != 0) {
        throw std::runtime_error("Failed to install SIGALRM handler");
    }

    // Install SIGXCPU handler for CPU time limit
    struct sigaction sa_cpu;
    std::memset(&sa_cpu, 0, sizeof(sa_cpu));
    sa_cpu.sa_handler = handleCpuLimit;
    sa_cpu.sa_flags = SA_RESTART;
    sigemptyset(&sa_cpu.sa_mask);

    if (sigaction(SIGXCPU, &sa_cpu, nullptr) != 0) {
        throw std::runtime_error("Failed to install SIGXCPU handler");
    }

    initialized_ = true;
}

bool ResourceLimiter::isInitialized() {
    return initialized_;
}

void ResourceLimiter::setExecutionTimeout(unsigned int seconds) {
    if (!initialized_) {
        installSignalHandlers();
    }

    timeout_triggered_ = false;

    // Set alarm for execution timeout
    alarm(seconds);
}

void ResourceLimiter::clearTimeout() {
    // Cancel any pending alarm
    alarm(0);
    timeout_triggered_ = false;
}

void ResourceLimiter::setMemoryLimit(size_t megabytes) {
    struct rlimit limit;
    limit.rlim_cur = megabytes * 1024 * 1024;  // Convert MB to bytes
    limit.rlim_max = megabytes * 1024 * 1024;

    if (setrlimit(RLIMIT_AS, &limit) != 0) {
        throw std::runtime_error("Failed to set memory limit: " + std::string(std::strerror(errno)));
    }
}

void ResourceLimiter::setCpuTimeLimit(unsigned int seconds) {
    if (!initialized_) {
        installSignalHandlers();
    }

    struct rlimit limit;
    limit.rlim_cur = seconds;
    limit.rlim_max = seconds;

    if (setrlimit(RLIMIT_CPU, &limit) != 0) {
        throw std::runtime_error("Failed to set CPU time limit: " + std::string(std::strerror(errno)));
    }
}

void ResourceLimiter::disableAll() {
    clearTimeout();

    // Remove memory limit (set to maximum)
    struct rlimit limit;
    limit.rlim_cur = RLIM_INFINITY;
    limit.rlim_max = RLIM_INFINITY;

    setrlimit(RLIMIT_AS, &limit);
    setrlimit(RLIMIT_CPU, &limit);
}

void ResourceLimiter::handleAlarm(int sig) {
    (void)sig;  // Unused parameter
    timeout_triggered_ = true;

    // Note: We can't throw exceptions from signal handlers
    // The timeout will be detected when control returns to normal code
}

void ResourceLimiter::handleCpuLimit(int sig) {
    (void)sig;  // Unused parameter
    timeout_triggered_ = true;
}

} // namespace security
} // namespace naab
