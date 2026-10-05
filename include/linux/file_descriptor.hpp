#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "common/result.hpp"

namespace pilot {

/// RAII owner of a POSIX file descriptor. Move-only; closes on destruction.
class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /// Give up ownership without closing.
    [[nodiscard]] int release() noexcept {
        const int old = fd_;
        fd_ = -1;
        return old;
    }

    /// Close the current descriptor (if any) and take ownership of `fd`.
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);  // never retried: on Linux the fd is released even on EINTR
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};

/// Write the whole buffer, handling short writes and EINTR.
inline Result<void> writeAll(int fd, std::string_view data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return makeError(ErrorCode::InternalError, "write failed: " + errnoMessage(errno), errno);
        }
        written += static_cast<std::size_t>(n);
    }
    return Result<void>::success();
}

/// Read until EOF (or `maxBytes`), handling short reads and EINTR.
inline Result<std::string> readAll(int fd, std::size_t maxBytes) {
    std::string out;
    char buffer[4096];
    while (out.size() < maxBytes) {
        const std::size_t want = std::min(sizeof(buffer), maxBytes - out.size());
        const ssize_t n = ::read(fd, buffer, want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return makeError(ErrorCode::InternalError, "read failed: " + errnoMessage(errno), errno);
        }
        if (n == 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
}

/// Open + read a (small) text file. The returned Error carries the errno.
inline Result<std::string> readTextFile(const std::filesystem::path& path, std::size_t maxBytes) {
    FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) {
        return makeError(ErrorCode::ProcReadFailed,
                         "cannot open " + path.string() + ": " + errnoMessage(errno), errno);
    }
    auto content = readAll(fd.get(), maxBytes);
    if (!content) {
        return makeError(ErrorCode::ProcReadFailed,
                         "cannot read " + path.string() + ": " + content.error().message,
                         content.error().sysErrno);
    }
    return content;
}

}  // namespace pilot
