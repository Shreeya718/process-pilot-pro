#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.hpp"

namespace pilot {

struct ThermalZone {
    std::string type;
    double celsius{0.0};
};

/// Read-only view of kernel-exposed hardware information.
struct SystemSnapshot {
    std::string onlineCpuList;                  // e.g. "0-7"
    std::size_t onlineCpuCount{0};
    std::optional<unsigned long> cpuFrequencyKhz;  // cpu0 scaling_cur_freq (absent in many VMs)
    std::vector<ThermalZone> thermalZones;      // /sys/class/thermal/thermal_zone*
    std::vector<std::string> networkInterfaces; // /sys/class/net/*
};

/// Userspace access to /sys (sysfs).
///
/// This project does NOT implement a kernel driver. Drivers publish device
/// state through sysfs attributes (plain text files under /sys/class and
/// /sys/devices); this reader consumes those attributes read-only, which is
/// how userspace daemons interact with the device model.
class SysfsReader {
public:
    explicit SysfsReader(std::filesystem::path root = "/sys");

    /// Fails only when the online-CPU list cannot be read at all; optional
    /// attributes (frequency, thermal) are simply left empty when absent.
    [[nodiscard]] Result<SystemSnapshot> snapshot() const;

    /// Entries of /sys/class/<className> (sorted); empty if the class does not exist.
    [[nodiscard]] std::vector<std::string> listClass(const std::string& className) const;

    /// Read one attribute below the sysfs root. Paths containing ".." are rejected.
    [[nodiscard]] Result<std::string> readAttribute(const std::filesystem::path& relative) const;

    /// Number of CPUs in a kernel CPU list such as "0-3,6,8-9" (-> 7).
    [[nodiscard]] static std::optional<std::size_t> countCpuList(std::string_view list);

private:
    std::filesystem::path root_;
};

}  // namespace pilot
