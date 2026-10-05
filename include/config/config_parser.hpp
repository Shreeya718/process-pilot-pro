#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "common/result.hpp"
#include "config/service_config.hpp"

namespace pilot {

/// Parses the INI-like service configuration format.
///
///   # comment                      (comments must be on their own line)
///   [ServiceName]
///   executable       = /path/to/bin
///   args             = --flag "quoted value"
///   dependencies     = Logger, Database      ("[Logger]" brackets are accepted too)
///   restart_policy   = never | on_failure | always
///   max_restarts     = 5
///   restart_delay_ms = 1000
///   restart_window_s = 60
///   working_dir      = /srv/app
///   enabled          = true
///   health_check     = process | none
///   health_interval_ms = 2000
///   max_rss_kb       = 0
///   stop_timeout_ms  = 5000
///
/// Responsibility: read + validate syntax and fields. It never starts processes.
class ConfigParser {
public:
    [[nodiscard]] Result<std::vector<ServiceDefinition>> parseFile(const std::filesystem::path& path) const;
    [[nodiscard]] Result<std::vector<ServiceDefinition>> parseString(
        std::string_view content, std::string_view sourceName = "<string>") const;

    /// Shell-like argument splitting with '...' and "..." quoting (no expansion).
    [[nodiscard]] static Result<std::vector<std::string>> splitArguments(std::string_view text);
};

}  // namespace pilot
