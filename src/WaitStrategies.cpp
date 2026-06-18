#include "WaitStrategies.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <variant>

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

namespace testcontainers {
namespace detail {

namespace {

using Clock = std::chrono::steady_clock;

/// Count non-overlapping occurrences of `needle` in `haystack`.
std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

/// Poll the container's logs every ~200ms until `text` has appeared `times`
/// times in the selected stream(s), or the deadline passes (then throw).
void wait_for_log(DockerClient& client, const std::string& id, const wait::LogMessage& cond,
                  Clock::time_point deadline) {
    using Source = wait::LogMessage::Source;

    LogOptions opts;
    opts.include_stdout = cond.source != Source::Stderr;
    opts.include_stderr = cond.source != Source::Stdout;
    opts.follow = false;
    opts.tail = "all";

    const int needed = cond.times < 1 ? 1 : cond.times;

    for (;;) {
        const ContainerLogs logs = client.logs(id, opts);

        std::size_t seen = 0;
        if (cond.source != Source::Stderr) {
            seen += count_occurrences(logs.stdout_data, cond.text);
        }
        if (cond.source != Source::Stdout) {
            seen += count_occurrences(logs.stderr_data, cond.text);
        }
        if (seen >= static_cast<std::size_t>(needed)) {
            return;
        }

        if (Clock::now() >= deadline) {
            throw DockerError("Timed out waiting for log message \"" + cond.text + "\" (" +
                              std::to_string(seen) + "/" + std::to_string(needed) +
                              " occurrences) in container " + id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

} // namespace

void wait_until_ready(DockerClient& client, const std::string& id,
                      const std::vector<WaitFor>& waits, std::chrono::milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;

    for (const WaitFor& w : waits) {
        std::visit(
            [&](const auto& cond) {
                using T = std::decay_t<decltype(cond)>;
                if constexpr (std::is_same_v<T, wait::None>) {
                    // nothing to do
                } else if constexpr (std::is_same_v<T, wait::LogMessage>) {
                    wait_for_log(client, id, cond, deadline);
                } else if constexpr (std::is_same_v<T, wait::Duration>) {
                    // Clamp the sleep to the shared deadline.
                    const Clock::time_point wake = Clock::now() + cond.value;
                    std::this_thread::sleep_until(wake < deadline ? wake : deadline);
                    if (Clock::now() >= deadline && wake > deadline) {
                        throw DockerError("Timed out during wait::Duration for container " + id);
                    }
                }
            },
            w);

        if (Clock::now() >= deadline) {
            // A condition may have just been satisfied at the deadline; only the
            // next condition (if any) would observe the expiry, so re-check here
            // to fail fast rather than enter the next strategy with no budget.
            if (&w != &waits.back()) {
                throw DockerError("Startup timeout exceeded while waiting for container " + id);
            }
        }
    }
}

} // namespace detail
} // namespace testcontainers
