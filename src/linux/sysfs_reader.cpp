#include "linux/sysfs_reader.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>
#include <thread>

#include "linux/file_descriptor.hpp"

namespace pilot {

namespace {

constexpr std::size_t kMaxSysfsFile = 64 * 1024;

std::string trimTrailing(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.pop_back();
    return s;
}

bool parseULong(std::string_view text, unsigned long& out) {
    if (text.empty()) return false;
    unsigned long v = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || ptr != text.data() + text.size()) return false;
    out = v;
    return true;
}

}  // namespace

SysfsReader::SysfsReader(std::filesystem::path root) : root_(std::move(root)) {}

Result<std::string> SysfsReader::readAttribute(const std::filesystem::path& relative) const {
    for (const auto& part : relative) {
        if (part == "..") {
            return makeError(ErrorCode::InvalidConfiguration, "sysfs path must not contain '..'");
        }
    }
    auto content = readTextFile(root_ / relative, kMaxSysfsFile);
    if (!content) {
        return content.error();
    }
    return trimTrailing(std::move(content.value()));
}

std::optional<std::size_t> SysfsReader::countCpuList(std::string_view list) {
    while (!list.empty() && std::isspace(static_cast<unsigned char>(list.back())) != 0) list.remove_suffix(1);
    if (list.empty()) return std::nullopt;

    std::size_t total = 0;
    while (!list.empty()) {
        const auto comma = list.find(',');
        const std::string_view item = list.substr(0, comma);
        const auto dash = item.find('-');
        unsigned long lo = 0;
        unsigned long hi = 0;
        if (dash == std::string_view::npos) {
            if (!parseULong(item, lo)) return std::nullopt;
            hi = lo;
        } else if (!parseULong(item.substr(0, dash), lo) || !parseULong(item.substr(dash + 1), hi) || hi < lo) {
            return std::nullopt;
        }
        total += static_cast<std::size_t>(hi - lo + 1);
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return total;
}

std::vector<std::string> SysfsReader::listClass(const std::string& className) const {
    std::vector<std::string> names;
    std::error_code ec;
    std::filesystem::directory_iterator it(root_ / "class" / className, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        names.push_back(it->path().filename().string());
        it.increment(ec);
    }
    std::sort(names.begin(), names.end());
    return names;
}

Result<SystemSnapshot> SysfsReader::snapshot() const {
    SystemSnapshot snap;

    auto online = readAttribute("devices/system/cpu/online");
    if (!online) {
        return makeError(ErrorCode::ProcReadFailed, "cannot determine online CPUs: " + online.error().message,
                         online.error().sysErrno);
    }
    snap.onlineCpuList = online.value();
    const auto count = countCpuList(snap.onlineCpuList);
    snap.onlineCpuCount = count.value_or(std::max<std::size_t>(1, std::thread::hardware_concurrency()));

    if (auto freq = readAttribute("devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"); freq) {
        unsigned long khz = 0;
        if (parseULong(freq.value(), khz)) snap.cpuFrequencyKhz = khz;
    }

    for (const auto& zone : listClass("thermal")) {
        if (zone.rfind("thermal_zone", 0) != 0) continue;
        auto type = readAttribute(std::filesystem::path("class/thermal") / zone / "type");
        auto temp = readAttribute(std::filesystem::path("class/thermal") / zone / "temp");
        if (!temp) continue;
        long milli = 0;
        const std::string& t = temp.value();
        const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), milli);
        if (ec != std::errc() || ptr != t.data() + t.size()) continue;
        snap.thermalZones.push_back({type ? type.value() : zone, static_cast<double>(milli) / 1000.0});
    }

    snap.networkInterfaces = listClass("net");
    return snap;
}

}  // namespace pilot
