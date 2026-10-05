#include "logging/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace pilot {

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARNING";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
    }
    return "UNKNOWN";
}

std::optional<LogLevel> parseLogLevel(std::string_view text) {
    std::string t(text);
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (t == "DEBUG") return LogLevel::Debug;
    if (t == "INFO") return LogLevel::Info;
    if (t == "WARNING" || t == "WARN") return LogLevel::Warning;
    if (t == "ERROR") return LogLevel::Error;
    if (t == "CRITICAL") return LogLevel::Critical;
    return std::nullopt;
}

Logger& Logger::instance() {
    static Logger logger;  // thread-safe initialisation (C++11 magic statics)
    return logger;
}

void Logger::setConsoleEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    consoleEnabled_ = enabled;
}

Result<void> Logger::setLogFile(const std::filesystem::path& path) {
    FileDescriptor fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644));
    if (!fd.valid()) {
        return makeError(ErrorCode::InternalError,
                         "cannot open log file '" + path.string() + "': " + errnoMessage(errno), errno);
    }
    std::scoped_lock lock(mutex_);
    file_ = std::move(fd);
    return Result<void>::success();
}

void Logger::closeLogFile() {
    std::scoped_lock lock(mutex_);
    file_.reset();
}

std::string Logger::formatLine(LogLevel level, std::string_view component, std::string_view message,
                               pid_t pid, std::chrono::system_clock::time_point when) {
    const std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32] = {0};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    std::string line;
    line.reserve(64 + component.size() + message.size());
    line += '[';
    line += stamp;
    line += "] [";
    line += toString(level);
    line += "] [";
    line += component;
    line += ']';
    if (pid > 0) {
        line += " [PID ";
        line += std::to_string(pid);
        line += ']';
    }
    line += ' ';
    line += message;
    line += '\n';
    return line;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message, pid_t pid) {
    if (level < level_.load()) {
        return;
    }
    const std::string line = formatLine(level, component, message, pid, std::chrono::system_clock::now());

    std::scoped_lock lock(mutex_);
    if (consoleEnabled_) {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }
    if (file_.valid()) {
        // A failing log file must never crash the supervisor: the error is dropped on purpose
        // (there is nowhere left to report it).
        if (auto r = writeAll(file_.get(), line); !r) {
            file_.reset();
        }
    }
}

}  // namespace pilot
