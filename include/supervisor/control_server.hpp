#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "common/result.hpp"
#include "linux/file_descriptor.hpp"

namespace pilot {

/// Default control-socket location: /tmp/process-pilot-<uid>.sock
std::filesystem::path defaultSocketPath();

/// Client side: send one command line, return the complete reply.
/// Fails with ProcessNotFound when no supervisor is listening on `socketPath`.
[[nodiscard]] Result<std::string> sendControlCommand(const std::filesystem::path& socketPath,
                                                     const std::string& command,
                                                     std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

/// Unix-domain-socket IPC endpoint: lets `process-pilot status|stop|restart|monitor`
/// talk to the running supervisor. Protocol: one text line in, text reply out, close.
///
/// Threading: one dedicated thread blocks in poll() on {listen socket, wake pipe};
/// stop() writes to the wake pipe, so shutdown is immediate and nothing busy-waits.
/// The handler runs on the control thread.
class ControlServer {
public:
    using Handler = std::function<std::string(const std::string&)>;

    ControlServer(std::filesystem::path socketPath, Handler handler);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    /// Binds the socket (refusing to steal it from a live supervisor) and starts the thread.
    [[nodiscard]] Result<void> start();
    void stop();

private:
    void serve();
    void handleClient(FileDescriptor client);

    std::filesystem::path path_;
    Handler handler_;
    FileDescriptor listenFd_;
    FileDescriptor wakeRead_;
    FileDescriptor wakeWrite_;
    std::thread thread_;
    bool bound_{false};
};

}  // namespace pilot
