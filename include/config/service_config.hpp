#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.hpp"

namespace pilot {

enum class RestartMode { Never, OnFailure, Always };
enum class HealthCheckMode { None, Process };

/// Restart behaviour of one service (see RestartPolicy for the algorithm).
struct RestartConfig {
    RestartMode mode{RestartMode::OnFailure};
    unsigned maxRestarts{5};                    // failures tolerated inside the window
    std::chrono::milliseconds delay{1000};      // pause before each restart
    std::chrono::seconds window{60};            // crash-loop window; 0 = whole lifetime
};

/// Health-check behaviour of one service (see HealthMonitor).
struct HealthCheckConfig {
    HealthCheckMode mode{HealthCheckMode::Process};
    std::chrono::milliseconds interval{2000};
    unsigned long maxRssKb{0};                  // 0 = unlimited
};

/// One service as declared in the configuration file.
struct ServiceDefinition {
    std::string name;
    std::string executable;
    std::vector<std::string> arguments;
    std::vector<std::string> dependencies;
    std::string workingDirectory;               // empty = inherit
    RestartConfig restart;
    HealthCheckConfig health;
    std::chrono::milliseconds stopTimeout{5000};  // SIGTERM -> SIGKILL escalation
    bool enabled{true};
};

std::string_view toString(RestartMode mode) noexcept;
std::string_view toString(HealthCheckMode mode) noexcept;
std::optional<RestartMode> parseRestartMode(std::string_view text);
std::optional<HealthCheckMode> parseHealthCheckMode(std::string_view text);

bool isValidServiceName(std::string_view name) noexcept;

/// Field-level validation of a single definition.
Result<void> validateServiceDefinition(const ServiceDefinition& service);

/// Cross-service validation (duplicate names, enabled service depending on a disabled one).
/// Missing dependencies and cycles are the DependencyGraph's job.
Result<void> validateServiceSet(const std::vector<ServiceDefinition>& services);

}  // namespace pilot
