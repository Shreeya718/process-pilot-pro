#include "linux/proc_reader.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <vector>

#include "linux/file_descriptor.hpp"

namespace pilot {

namespace {

constexpr std::size_t kMaxProcFile = 256 * 1024;

std::vector<std::string_view> splitWhitespace(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
        const std::size_t start = i;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) == 0) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

template <typename T>
bool parseNumber(std::string_view text, T& out) {
    if (text.empty()) return false;
    T value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
    return s;
}

}  // namespace

ProcReader::ProcReader(std::filesystem::path root) : root_(std::move(root)) {}

Error ProcReader::mapReadError(const Error& e, const std::filesystem::path& path) {
    switch (e.sysErrno) {
        case ENOENT:
        case ESRCH:
        case ENOTDIR:
            return makeError(ErrorCode::ProcessNotFound, "process vanished while reading " + path.string(),
                             e.sysErrno);
        case EACCES:
        case EPERM:
            return makeError(ErrorCode::PermissionDenied, "permission denied reading " + path.string(),
                             e.sysErrno);
        default:
            return makeError(ErrorCode::ProcReadFailed, e.message, e.sysErrno);
    }
}

Result<void> ProcReader::parseStat(std::string_view content, ProcessMetrics& out) {
    const auto malformed = [](const std::string& why) {
        return makeError(ErrorCode::ProcReadFailed, "malformed /proc/<pid>/stat: " + why);
    };

    const auto open = content.find('(');
    const auto close = content.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        return malformed("missing comm field");
    }

    long pidValue = 0;
    if (!parseNumber(trimView(content.substr(0, open)), pidValue)) {
        return malformed("bad pid field");
    }

    // After ") " the fields are numbered from 3 (state): token index = field - 3.
    const auto tokens = splitWhitespace(content.substr(close + 1));
    if (tokens.size() < 22 || tokens[0].size() != 1) {
        return malformed("too few fields (" + std::to_string(tokens.size()) + ")");
    }

    long long ppid = 0;
    long long threads = 0;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    unsigned long long startTime = 0;
    unsigned long long vsizeBytes = 0;
    long long rssPages = 0;
    if (!parseNumber(tokens[1], ppid) || !parseNumber(tokens[11], utime) || !parseNumber(tokens[12], stime) ||
        !parseNumber(tokens[17], threads) || !parseNumber(tokens[19], startTime) ||
        !parseNumber(tokens[20], vsizeBytes) || !parseNumber(tokens[21], rssPages)) {
        return malformed("non-numeric field");
    }

    out.pid = static_cast<pid_t>(pidValue);
    out.name = std::string(content.substr(open + 1, close - open - 1));
    out.state = tokens[0][0];
    out.parentPid = static_cast<pid_t>(ppid);
    out.userTicks = utime;
    out.systemTicks = stime;
    out.threads = static_cast<long>(threads);
    out.startTimeTicks = startTime;
    out.virtualKb = static_cast<unsigned long>(vsizeBytes / 1024ULL);
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    out.residentKb = (rssPages > 0 && pageSize > 0)
                         ? static_cast<unsigned long>(static_cast<unsigned long long>(rssPages) *
                                                      static_cast<unsigned long long>(pageSize) / 1024ULL)
                         : 0UL;
    return Result<void>::success();
}

void ProcReader::parseStatus(std::string_view content, ProcessMetrics& out) {
    while (!content.empty()) {
        const auto nl = content.find('\n');
        const std::string_view line = content.substr(0, nl);
        content = (nl == std::string_view::npos) ? std::string_view{} : content.substr(nl + 1);

        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, colon);
        const auto fields = splitWhitespace(line.substr(colon + 1));
        if (fields.empty()) continue;

        unsigned long number = 0;
        if (key == "VmRSS" && parseNumber(fields[0], number)) {
            out.residentKb = number;
        } else if (key == "VmSize" && parseNumber(fields[0], number)) {
            out.virtualKb = number;
        } else if (key == "Threads" && parseNumber(fields[0], number)) {
            out.threads = static_cast<long>(number);
        }
    }
}

std::string ProcReader::parseCmdline(std::string_view raw) {
    std::string out(raw);
    std::replace(out.begin(), out.end(), '\0', ' ');
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

Result<ProcessMetrics> ProcReader::readProcess(pid_t pid) const {
    if (pid <= 0) {
        return makeError(ErrorCode::ProcessNotFound, "invalid pid " + std::to_string(pid));
    }
    const std::filesystem::path dir = root_ / std::to_string(pid);

    const auto statPath = dir / "stat";
    auto stat = readTextFile(statPath, kMaxProcFile);
    if (!stat) {
        return mapReadError(stat.error(), statPath);
    }

    ProcessMetrics metrics;
    if (auto parsed = parseStat(stat.value(), metrics); !parsed) {
        return parsed.error();
    }

    const auto statusPath = dir / "status";
    auto status = readTextFile(statusPath, kMaxProcFile);
    if (status) {
        parseStatus(status.value(), metrics);
    } else if (const Error e = mapReadError(status.error(), statusPath); e.code == ErrorCode::ProcessNotFound) {
        return e;
    }

    const auto cmdPath = dir / "cmdline";
    auto cmd = readTextFile(cmdPath, kMaxProcFile);
    if (cmd) {
        metrics.commandLine = parseCmdline(cmd.value());
    } else if (const Error e = mapReadError(cmd.error(), cmdPath); e.code == ErrorCode::ProcessNotFound) {
        return e;
    }

    return metrics;
}

}  // namespace pilot
