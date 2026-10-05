#include "config/service_config.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace pilot {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

std::string_view toString(RestartMode mode) noexcept {
    switch (mode) {
        case RestartMode::Never: return "never";
        case RestartMode::OnFailure: return "on_failure";
        case RestartMode::Always: return "always";
    }
    return "unknown";
}

std::string_view toString(HealthCheckMode mode) noexcept {
    switch (mode) {
        case HealthCheckMode::None: return "none";
        case HealthCheckMode::Process: return "process";
    }
    return "unknown";
}

std::optional<RestartMode> parseRestartMode(std::string_view text) {
    const std::string t = lower(text);
    if (t == "never") return RestartMode::Never;
    if (t == "on_failure" || t == "on-failure" || t == "onfailure") return RestartMode::OnFailure;
    if (t == "always") return RestartMode::Always;
    return std::nullopt;
}

std::optional<HealthCheckMode> parseHealthCheckMode(std::string_view text) {
    const std::string t = lower(text);
    if (t == "none" || t == "off") return HealthCheckMode::None;
    if (t == "process" || t == "proc") return HealthCheckMode::Process;
    return std::nullopt;
}

bool isValidServiceName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
    });
}

Result<void> validateServiceDefinition(const ServiceDefinition& s) {
    const auto bad = [&s](const std::string& why) {
        return makeError(ErrorCode::InvalidConfiguration, "service '" + s.name + "': " + why);
    };
    if (!isValidServiceName(s.name)) {
        return makeError(ErrorCode::InvalidConfiguration,
                         "invalid service name '" + s.name + "' (use letters, digits, '_', '-', '.'; max 64)");
    }
    if (s.executable.empty()) {
        return bad("missing required key 'executable'");
    }
    for (const auto& dep : s.dependencies) {
        if (!isValidServiceName(dep)) {
            return bad("invalid dependency name '" + dep + "'");
        }
    }
    if (s.stopTimeout.count() <= 0) {
        return bad("stop_timeout_ms must be greater than 0");
    }
    if (s.health.mode == HealthCheckMode::Process && s.health.interval.count() < 100) {
        return bad("health_interval_ms must be at least 100");
    }
    return Result<void>::success();
}

Result<void> validateServiceSet(const std::vector<ServiceDefinition>& services) {
    std::unordered_map<std::string, const ServiceDefinition*> byName;
    for (const auto& s : services) {
        if (!byName.emplace(s.name, &s).second) {
            return makeError(ErrorCode::InvalidConfiguration, "duplicate service '" + s.name + "'");
        }
    }
    for (const auto& s : services) {
        if (!s.enabled) {
            continue;
        }
        for (const auto& dep : s.dependencies) {
            const auto it = byName.find(dep);
            if (it != byName.end() && !it->second->enabled) {
                return makeError(ErrorCode::InvalidConfiguration,
                                 "enabled service '" + s.name + "' depends on disabled service '" + dep + "'");
            }
        }
    }
    return Result<void>::success();
}

}  // namespace pilot
