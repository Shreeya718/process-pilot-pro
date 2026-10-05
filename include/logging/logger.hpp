#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "common/result.hpp"
#include "linux/file_descriptor.hpp"

namespace pilot {

enum class LogLevel { Debug, Info, Warning, Error, Critical };

std::string_view toString(LogLevel level) noexcept;
std::optional<LogLevel> parseLogLevel(std::string_view text);

/// Thread-safe logger writing to stderr and/or a log file.
///
///   [2026-10-05 12:10:43] [INFO] [Database] [PID 102] Started
///
/// One mutex serialises writers, so lines from different threads never
/// interleave. The log file is opened with O_APPEND|O_CLOEXEC so supervised
/// children never inherit it.
class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level) noexcept { level_.store(level); }
    [[nodiscard]] LogLevel level() const noexcept { return level_.load(); }

    void setConsoleEnabled(bool enabled);
    [[nodiscard]] Result<void> setLogFile(const std::filesystem::path& path);
    void closeLogFile();

    void log(LogLevel level, std::string_view component, std::string_view message, pid_t pid = 0);

    void debug(std::string_view component, std::string_view message, pid_t pid = 0) {
        log(LogLevel::Debug, component, message, pid);
    }
    void info(std::string_view component, std::string_view message, pid_t pid = 0) {
        log(LogLevel::Info, component, message, pid);
    }
    void warning(std::string_view component, std::string_view message, pid_t pid = 0) {
        log(LogLevel::Warning, component, message, pid);
    }
    void error(std::string_view component, std::string_view message, pid_t pid = 0) {
        log(LogLevel::Error, component, message, pid);
    }
    void critical(std::string_view component, std::string_view message, pid_t pid = 0) {
        log(LogLevel::Critical, component, message, pid);
    }

    /// Pure formatting helper (exposed for tests).
    static std::string formatLine(LogLevel level, std::string_view component, std::string_view message,
                                  pid_t pid, std::chrono::system_clock::time_point when);

private:
    Logger() = default;

    std::atomic<LogLevel> level_{LogLevel::Info};
    std::mutex mutex_;  // guards consoleEnabled_ and file_ and serialises output
    bool consoleEnabled_{true};
    FileDescriptor file_;
};

}  // namespace pilot
