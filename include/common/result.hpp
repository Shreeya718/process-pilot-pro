#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "common/error.hpp"

namespace pilot {

/// Minimal value-or-error type (a C++20 stand-in for C++23 std::expected).
///
///   Result<pid_t> r = manager.start(service);
///   if (!r) { log(r.error().describe()); return r.error(); }
///   use(r.value());
///
/// The class is [[nodiscard]]: silently ignoring a failure is a compile error.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : data_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<0>(data_); }
    const T& value() const& { return std::get<0>(data_); }
    T&& value() && { return std::get<0>(std::move(data_)); }

    [[nodiscard]] const Error& error() const { return std::get<1>(data_); }

private:
    std::variant<T, Error> data_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    static Result success() { return Result(); }

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const { return error_.value(); }

private:
    std::optional<Error> error_;
};

}  // namespace pilot
