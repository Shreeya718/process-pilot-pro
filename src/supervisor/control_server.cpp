#include "supervisor/control_server.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>

#include "logging/logger.hpp"

namespace pilot {

namespace {

constexpr std::size_t kMaxCommandBytes = 4096;
constexpr std::size_t kMaxReplyBytes = 1024 * 1024;

Result<sockaddr_un> makeAddress(const std::filesystem::path& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string text = path.string();
    if (text.empty() || text.size() >= sizeof(addr.sun_path)) {
        return makeError(ErrorCode::InvalidConfiguration,
                         "socket path is empty or longer than " + std::to_string(sizeof(addr.sun_path) - 1) +
                             " bytes: '" + text + "'");
    }
    std::memcpy(addr.sun_path, text.c_str(), text.size() + 1);
    return addr;
}

void setTimeouts(int fd, std::chrono::milliseconds timeout) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

Result<void> sendAll(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);  // no SIGPIPE
        if (n < 0) {
            if (errno == EINTR) continue;
            return makeError(ErrorCode::InternalError, "send failed: " + errnoMessage(errno), errno);
        }
        sent += static_cast<std::size_t>(n);
    }
    return Result<void>::success();
}

}  // namespace

std::filesystem::path defaultSocketPath() {
    return "/tmp/process-pilot-" + std::to_string(::getuid()) + ".sock";
}

Result<std::string> sendControlCommand(const std::filesystem::path& socketPath, const std::string& command,
                                       std::chrono::milliseconds timeout) {
    auto addr = makeAddress(socketPath);
    if (!addr) return addr.error();

    FileDescriptor fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd.valid()) {
        return makeError(ErrorCode::InternalError, "socket failed: " + errnoMessage(errno), errno);
    }
    setTimeouts(fd.get(), timeout);

    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr.value()), sizeof(sockaddr_un)) != 0) {
        const int e = errno;
        if (e == ENOENT || e == ECONNREFUSED) {
            return makeError(ErrorCode::ProcessNotFound,
                             "no running supervisor at '" + socketPath.string() + "'", e);
        }
        if (e == EACCES || e == EPERM) {
            return makeError(ErrorCode::PermissionDenied,
                             "not permitted to use control socket '" + socketPath.string() + "'", e);
        }
        return makeError(ErrorCode::InternalError, "connect failed: " + errnoMessage(e), e);
    }

    if (auto r = sendAll(fd.get(), command + "\n"); !r) return r.error();
    ::shutdown(fd.get(), SHUT_WR);  // tells the server the request is complete

    auto reply = readAll(fd.get(), kMaxReplyBytes);
    if (!reply) return reply.error();
    return reply;
}

ControlServer::ControlServer(std::filesystem::path socketPath, Handler handler)
    : path_(std::move(socketPath)), handler_(std::move(handler)) {}

ControlServer::~ControlServer() { stop(); }

Result<void> ControlServer::start() {
    auto addr = makeAddress(path_);
    if (!addr) return addr.error();

    // Refuse to hijack the socket of a live supervisor; remove a stale socket file.
    if (auto ping = sendControlCommand(path_, "ping", std::chrono::milliseconds(1000)); ping) {
        return makeError(ErrorCode::InvalidState, "another supervisor is already running (" + path_.string() + ")");
    }
    struct stat st {};
    if (::lstat(path_.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            return makeError(ErrorCode::InvalidConfiguration, "'" + path_.string() + "' exists and is not a socket");
        }
        ::unlink(path_.c_str());
    }

    FileDescriptor listenFd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!listenFd.valid()) {
        return makeError(ErrorCode::InternalError, "socket failed: " + errnoMessage(errno), errno);
    }
    if (::bind(listenFd.get(), reinterpret_cast<const sockaddr*>(&addr.value()), sizeof(sockaddr_un)) != 0) {
        return makeError(ErrorCode::InternalError,
                         "cannot bind control socket '" + path_.string() + "': " + errnoMessage(errno), errno);
    }
    bound_ = true;
    ::chmod(path_.c_str(), 0600);  // only the owner may control the supervisor
    if (::listen(listenFd.get(), 8) != 0) {
        const int e = errno;
        ::unlink(path_.c_str());
        bound_ = false;
        return makeError(ErrorCode::InternalError, "listen failed: " + errnoMessage(e), e);
    }

    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        const int e = errno;
        ::unlink(path_.c_str());
        bound_ = false;
        return makeError(ErrorCode::InternalError, "pipe2 failed: " + errnoMessage(e), e);
    }
    wakeRead_.reset(fds[0]);
    wakeWrite_.reset(fds[1]);
    listenFd_ = std::move(listenFd);
    thread_ = std::thread(&ControlServer::serve, this);
    Logger::instance().info("Control", "listening on " + path_.string());
    return Result<void>::success();
}

void ControlServer::stop() {
    if (thread_.joinable()) {
        const char byte = 'x';
        const ssize_t w = ::write(wakeWrite_.get(), &byte, 1);
        (void)w;
        thread_.join();
    }
    listenFd_.reset();
    if (bound_) {
        ::unlink(path_.c_str());
        bound_ = false;
    }
}

void ControlServer::serve() {
    pollfd fds[2];
    fds[0] = pollfd{listenFd_.get(), POLLIN, 0};
    fds[1] = pollfd{wakeRead_.get(), POLLIN, 0};
    for (;;) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        const int rc = ::poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            Logger::instance().error("Control", "poll failed: " + errnoMessage(errno));
            return;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            return;  // stop() requested
        }
        if ((fds[0].revents & POLLIN) != 0) {
            const int c = ::accept4(listenFd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) {
                if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) {
                    Logger::instance().error("Control", "accept failed: " + errnoMessage(errno));
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // avoid a hot loop on EMFILE
                }
                continue;
            }
            handleClient(FileDescriptor(c));
        }
    }
}

void ControlServer::handleClient(FileDescriptor client) {
    setTimeouts(client.get(), std::chrono::milliseconds(2000));

    std::string request;
    char buffer[512];
    while (request.size() < kMaxCommandBytes && request.find('\n') == std::string::npos) {
        const ssize_t n = ::read(client.get(), buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;  // EOF, timeout or error
        request.append(buffer, static_cast<std::size_t>(n));
    }
    const auto nl = request.find('\n');
    if (nl != std::string::npos) request.resize(nl);

    std::string reply;
    try {
        reply = handler_(request);
    } catch (const std::exception& ex) {
        reply = std::string("ERROR: internal error: ") + ex.what() + "\n";
    } catch (...) {
        reply = "ERROR: internal error\n";
    }
    if (auto r = sendAll(client.get(), reply); !r) {
        Logger::instance().debug("Control", "reply not delivered: " + r.error().message);
    }
}

}  // namespace pilot
