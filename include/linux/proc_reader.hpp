#pragma once

#include <sys/types.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "common/result.hpp"
#include "monitor/process_metrics.hpp"

namespace pilot {

/// Reads per-process information from procfs.
///
/// Robustness rules (a process can vanish between any two reads):
///   * /proc/<pid>/stat is mandatory; ENOENT/ESRCH/ENOTDIR -> ProcessNotFound,
///     EACCES/EPERM -> PermissionDenied, anything else -> ProcReadFailed;
///   * status and cmdline are best-effort (but "process vanished" is propagated);
///   * short reads are handled by readAll(); malformed content yields ProcReadFailed,
///     never undefined behaviour.
/// The root directory is injectable so tests can point it at a fake tree.
class ProcReader {
public:
    explicit ProcReader(std::filesystem::path root = "/proc");

    [[nodiscard]] Result<ProcessMetrics> readProcess(pid_t pid) const;

    /// Parse the contents of /proc/<pid>/stat. The comm field may contain spaces
    /// and parentheses, so the parser anchors on the *last* ')'.
    [[nodiscard]] static Result<void> parseStat(std::string_view content, ProcessMetrics& out);

    /// Parse VmRSS / VmSize / Threads from /proc/<pid>/status (missing keys are ignored).
    static void parseStatus(std::string_view content, ProcessMetrics& out);

    /// Convert NUL-separated argv into a space separated string.
    [[nodiscard]] static std::string parseCmdline(std::string_view raw);

private:
    [[nodiscard]] static Error mapReadError(const Error& e, const std::filesystem::path& path);

    std::filesystem::path root_;
};

}  // namespace pilot
