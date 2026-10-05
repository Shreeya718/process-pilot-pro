#include "config/config_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>

#include "linux/file_descriptor.hpp"

namespace pilot {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
    return s;
}

std::string lowerCopy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::optional<unsigned long long> parseUnsigned(std::string_view text, unsigned long long maxValue) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    unsigned long long value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size() || value > maxValue) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parseBool(std::string_view text) {
    const std::string t = lowerCopy(trim(text));
    if (t == "true" || t == "yes" || t == "on" || t == "1") return true;
    if (t == "false" || t == "no" || t == "off" || t == "0") return false;
    return std::nullopt;
}

std::vector<std::string> parseNameList(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == '[') text.remove_prefix(1);
    if (!text.empty() && text.back() == ']') text.remove_suffix(1);
    std::vector<std::string> names;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const std::string_view item = trim(text.substr(0, comma));
        if (!item.empty() && std::find(names.begin(), names.end(), item) == names.end()) {
            names.emplace_back(item);
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return names;
}

constexpr unsigned long long kMaxMillis = 24ULL * 60 * 60 * 1000;

}  // namespace

Result<std::vector<std::string>> ConfigParser::splitArguments(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    bool inToken = false;
    char quote = '\0';
    for (const char c : text) {
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current.push_back(c);
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
            inToken = true;
        } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (inToken) {
                out.push_back(std::move(current));
                current.clear();
                inToken = false;
            }
        } else {
            current.push_back(c);
            inToken = true;
        }
    }
    if (quote != '\0') {
        return makeError(ErrorCode::InvalidConfiguration, "unterminated quote in arguments");
    }
    if (inToken) {
        out.push_back(std::move(current));
    }
    return out;
}

Result<std::vector<ServiceDefinition>> ConfigParser::parseFile(const std::filesystem::path& path) const {
    auto content = readTextFile(path, 4U * 1024U * 1024U);
    if (!content) {
        return makeError(ErrorCode::InvalidConfiguration,
                         "cannot read configuration file '" + path.string() + "': " +
                             errnoMessage(content.error().sysErrno));
    }
    return parseString(content.value(), path.string());
}

Result<std::vector<ServiceDefinition>> ConfigParser::parseString(std::string_view content,
                                                                 std::string_view sourceName) const {
    std::vector<ServiceDefinition> services;
    std::optional<ServiceDefinition> current;
    std::size_t currentLine = 0;
    std::size_t lineNo = 0;

    const auto fail = [&](std::size_t line, const std::string& why) {
        return makeError(ErrorCode::InvalidConfiguration,
                         std::string(sourceName) + ":" + std::to_string(line) + ": " + why);
    };

    const auto finishSection = [&]() -> Result<void> {
        if (!current) return Result<void>::success();
        auto valid = validateServiceDefinition(*current);
        if (!valid) {
            return fail(currentLine, valid.error().message);
        }
        services.push_back(std::move(*current));
        current.reset();
        return Result<void>::success();
    };

    std::size_t pos = 0;
    while (pos <= content.size()) {
        const auto nl = content.find('\n', pos);
        std::string_view line = content.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? content.size() + 1 : nl + 1;
        ++lineNo;

        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;

        if (line.front() == '[') {
            if (line.back() != ']') return fail(lineNo, "malformed section header '" + std::string(line) + "'");
            if (auto r = finishSection(); !r) return r.error();
            const std::string name(trim(line.substr(1, line.size() - 2)));
            if (!isValidServiceName(name)) return fail(lineNo, "invalid service name '" + name + "'");
            const bool duplicate = std::any_of(services.begin(), services.end(),
                                               [&](const ServiceDefinition& s) { return s.name == name; });
            if (duplicate) return fail(lineNo, "duplicate service section [" + name + "]");
            current.emplace();
            current->name = name;
            currentLine = lineNo;
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            return fail(lineNo, "expected 'key = value' but found '" + std::string(line) + "'");
        }
        if (!current) return fail(lineNo, "key/value pair outside of a [service] section");

        const std::string key = lowerCopy(trim(line.substr(0, eq)));
        const std::string_view value = trim(line.substr(eq + 1));
        ServiceDefinition& s = *current;

        if (key == "executable") {
            s.executable = std::string(value);
        } else if (key == "args" || key == "arguments") {
            auto args = splitArguments(value);
            if (!args) return fail(lineNo, args.error().message);
            s.arguments = std::move(args.value());
        } else if (key == "dependencies" || key == "depends_on") {
            s.dependencies = parseNameList(value);
        } else if (key == "restart_policy") {
            const auto mode = parseRestartMode(value);
            if (!mode) return fail(lineNo, "restart_policy must be never, on_failure or always (got '" + std::string(value) + "')");
            s.restart.mode = *mode;
        } else if (key == "max_restarts") {
            const auto n = parseUnsigned(value, 1000000);
            if (!n) return fail(lineNo, "max_restarts must be an integer between 0 and 1000000");
            s.restart.maxRestarts = static_cast<unsigned>(*n);
        } else if (key == "restart_delay_ms") {
            const auto n = parseUnsigned(value, kMaxMillis);
            if (!n) return fail(lineNo, "restart_delay_ms must be an integer between 0 and 86400000");
            s.restart.delay = std::chrono::milliseconds(static_cast<long long>(*n));
        } else if (key == "restart_window_s") {
            const auto n = parseUnsigned(value, 86400);
            if (!n) return fail(lineNo, "restart_window_s must be an integer between 0 and 86400");
            s.restart.window = std::chrono::seconds(static_cast<long long>(*n));
        } else if (key == "working_dir" || key == "working_directory") {
            s.workingDirectory = std::string(value);
        } else if (key == "enabled") {
            const auto b = parseBool(value);
            if (!b) return fail(lineNo, "enabled must be true or false");
            s.enabled = *b;
        } else if (key == "health_check") {
            const auto mode = parseHealthCheckMode(value);
            if (!mode) return fail(lineNo, "health_check must be 'process' or 'none'");
            s.health.mode = *mode;
        } else if (key == "health_interval_ms") {
            const auto n = parseUnsigned(value, kMaxMillis);
            if (!n) return fail(lineNo, "health_interval_ms must be an integer between 0 and 86400000");
            s.health.interval = std::chrono::milliseconds(static_cast<long long>(*n));
        } else if (key == "max_rss_kb") {
            const auto n = parseUnsigned(value, 1ULL << 40);
            if (!n) return fail(lineNo, "max_rss_kb must be a non-negative integer");
            s.health.maxRssKb = static_cast<unsigned long>(*n);
        } else if (key == "stop_timeout_ms") {
            const auto n = parseUnsigned(value, kMaxMillis);
            if (!n) return fail(lineNo, "stop_timeout_ms must be an integer between 0 and 86400000");
            s.stopTimeout = std::chrono::milliseconds(static_cast<long long>(*n));
        } else {
            return fail(lineNo, "unknown key '" + key + "'");
        }
    }

    if (auto r = finishSection(); !r) return r.error();

    if (services.empty()) {
        return makeError(ErrorCode::InvalidConfiguration,
                         std::string(sourceName) + ": no services defined");
    }
    if (auto r = validateServiceSet(services); !r) {
        return makeError(ErrorCode::InvalidConfiguration, std::string(sourceName) + ": " + r.error().message);
    }
    return services;
}

}  // namespace pilot
