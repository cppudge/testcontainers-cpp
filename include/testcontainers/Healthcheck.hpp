#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace testcontainers {

/// A container HEALTHCHECK definition, mirroring Docker's create-body
/// `Healthcheck` object.
///
/// The `test` vector follows Docker's tagged convention: the first element is
/// the kind (`"CMD"`, `"CMD-SHELL"`, or `"NONE"`) and the remaining elements are
/// the command/arguments. Build one with the static factories, then tune it with
/// the chainable, ref-qualified setters (same style as `GenericImage`).
///
/// A plain, copyable value type (no Boost/Asio leakage).
class Healthcheck {
public:
    /// `["CMD-SHELL", cmd]` — run `cmd` with the container's default shell.
    static Healthcheck cmd_shell(std::string cmd) {
        Healthcheck h;
        h.test_ = {"CMD-SHELL", std::move(cmd)};
        return h;
    }

    /// `["CMD", arg0, arg1, ...]` — exec the given argv directly (no shell).
    static Healthcheck cmd(std::vector<std::string> argv) {
        Healthcheck h;
        h.test_.reserve(argv.size() + 1);
        h.test_.emplace_back("CMD");
        for (auto& a : argv) {
            h.test_.push_back(std::move(a));
        }
        return h;
    }

    /// `["NONE"]` — disable any healthcheck inherited from the image.
    static Healthcheck none() {
        Healthcheck h;
        h.test_ = {"NONE"};
        return h;
    }

    // --- In-place, ref-qualified setters ---

    Healthcheck& with_interval(std::chrono::nanoseconds interval) & {
        interval_ = interval;
        return *this;
    }
    Healthcheck&& with_interval(std::chrono::nanoseconds interval) && {
        interval_ = interval;
        return std::move(*this);
    }

    Healthcheck& with_timeout(std::chrono::nanoseconds timeout) & {
        timeout_ = timeout;
        return *this;
    }
    Healthcheck&& with_timeout(std::chrono::nanoseconds timeout) && {
        timeout_ = timeout;
        return std::move(*this);
    }

    Healthcheck& with_retries(int retries) & {
        retries_ = retries;
        return *this;
    }
    Healthcheck&& with_retries(int retries) && {
        retries_ = retries;
        return std::move(*this);
    }

    Healthcheck& with_start_period(std::chrono::nanoseconds start_period) & {
        start_period_ = start_period;
        return *this;
    }
    Healthcheck&& with_start_period(std::chrono::nanoseconds start_period) && {
        start_period_ = start_period;
        return std::move(*this);
    }

    // --- Getters ---

    const std::vector<std::string>& test() const noexcept { return test_; }
    std::optional<std::chrono::nanoseconds> interval() const noexcept { return interval_; }
    std::optional<std::chrono::nanoseconds> timeout() const noexcept { return timeout_; }
    std::optional<std::chrono::nanoseconds> start_period() const noexcept { return start_period_; }
    std::optional<int> retries() const noexcept { return retries_; }

private:
    std::vector<std::string> test_;
    std::optional<std::chrono::nanoseconds> interval_;
    std::optional<std::chrono::nanoseconds> timeout_;
    std::optional<std::chrono::nanoseconds> start_period_;
    std::optional<int> retries_;
};

} // namespace testcontainers
