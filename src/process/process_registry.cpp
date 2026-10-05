#include "process/process_registry.hpp"

namespace pilot {

namespace {
Error notFound(const std::string& name) {
    return makeError(ErrorCode::ProcessNotFound, "unknown service '" + name + "'");
}
}  // namespace

Result<void> ProcessRegistry::registerService(const std::string& name) {
    std::scoped_lock lock(mutex_);
    if (byName_.contains(name)) {
        return makeError(ErrorCode::InvalidConfiguration, "service '" + name + "' is already registered");
    }
    ProcessInfo info;
    info.serviceName = name;
    byName_.emplace(name, std::move(info));
    order_.push_back(name);
    return Result<void>::success();
}

bool ProcessRegistry::contains(const std::string& name) const {
    std::scoped_lock lock(mutex_);
    return byName_.contains(name);
}

std::optional<ProcessInfo> ProcessRegistry::get(const std::string& name) const {
    std::scoped_lock lock(mutex_);
    const auto it = byName_.find(name);
    if (it == byName_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> ProcessRegistry::findByPid(pid_t pid) const {
    std::scoped_lock lock(mutex_);
    const auto it = pidIndex_.find(pid);
    if (it == pidIndex_.end()) return std::nullopt;
    return it->second;
}

Result<void> ProcessRegistry::transition(const std::string& name, ProcessState next) {
    std::scoped_lock lock(mutex_);
    const auto it = byName_.find(name);
    if (it == byName_.end()) return notFound(name);
    if (!isValidTransition(it->second.state, next)) {
        return makeError(ErrorCode::InvalidState,
                         "service '" + name + "': illegal transition " +
                             std::string(toString(it->second.state)) + " -> " + std::string(toString(next)));
    }
    it->second.state = next;
    return Result<void>::success();
}

Result<void> ProcessRegistry::markRunning(const std::string& name, pid_t pid,
                                          std::chrono::steady_clock::time_point startedAt) {
    std::scoped_lock lock(mutex_);
    const auto it = byName_.find(name);
    if (it == byName_.end()) return notFound(name);
    if (pid <= 0) {
        return makeError(ErrorCode::InvalidState, "service '" + name + "': invalid pid");
    }
    if (!isValidTransition(it->second.state, ProcessState::Running)) {
        return makeError(ErrorCode::InvalidState,
                         "service '" + name + "': illegal transition " +
                             std::string(toString(it->second.state)) + " -> RUNNING");
    }
    if (it->second.pid > 0) {
        pidIndex_.erase(it->second.pid);
    }
    it->second.state = ProcessState::Running;
    it->second.pid = pid;
    it->second.startTime = startedAt;
    pidIndex_[pid] = name;
    return Result<void>::success();
}

void ProcessRegistry::clearPid(const std::string& name) {
    std::scoped_lock lock(mutex_);
    const auto it = byName_.find(name);
    if (it == byName_.end()) return;
    if (it->second.pid > 0) {
        pidIndex_.erase(it->second.pid);
    }
    it->second.pid = 0;
}

Result<void> ProcessRegistry::incrementRestartCount(const std::string& name) {
    std::scoped_lock lock(mutex_);
    const auto it = byName_.find(name);
    if (it == byName_.end()) return notFound(name);
    ++it->second.restartCount;
    return Result<void>::success();
}

std::vector<ProcessInfo> ProcessRegistry::snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<ProcessInfo> out;
    out.reserve(order_.size());
    for (const auto& name : order_) {
        out.push_back(byName_.at(name));
    }
    return out;
}

}  // namespace pilot
