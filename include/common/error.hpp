#pragma once

#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace pilot {

/// Project-wide error categories. Every fallible operation reports one of
/// these together with a human readable message (see Error).
enum class ErrorCode {
    None,
    InvalidConfiguration,
    DependencyCycle,
    ForkFailed,
    ExecFailed,
    ProcessNotFound,
    PermissionDenied,
    InvalidState,
    RestartLimitExceeded,
    ProcReadFailed,  // also used for /sys read failures
    InternalError
};

constexpr std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None: return "None";
        case ErrorCode::InvalidConfiguration: return "InvalidConfiguration";
        case ErrorCode::DependencyCycle: return "DependencyCycle";
        case ErrorCode::ForkFailed: return "ForkFailed";
        case ErrorCode::ExecFailed: return "ExecFailed";
        case ErrorCode::ProcessNotFound: return "ProcessNotFound";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::InvalidState: return "InvalidState";
        case ErrorCode::RestartLimitExceeded: return "RestartLimitExceeded";
        case ErrorCode::ProcReadFailed: return "ProcReadFailed";
        case ErrorCode::InternalError: return "InternalError";
    }
    return "Unknown";
}

/// Thread-safe errno -> text conversion (std::strerror is not thread-safe).
inline std::string errnoMessage(int err) {
    return std::error_code(err, std::generic_category()).message();
}

/// Error value: category + context message + (optional) the errno that caused it.
struct Error {
    ErrorCode code{ErrorCode::InternalError};
    std::string message;
    int sysErrno{0};

    [[nodiscard]] std::string describe() const {
        return std::string(toString(code)) + ": " + message;
    }
};

inline Error makeError(ErrorCode code, std::string message, int sysErrno = 0) {
    return Error{code, std::move(message), sysErrno};
}

}  // namespace pilot
