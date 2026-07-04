#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace testcontainers {

/// Options for `GET /containers/{id}/logs`.
///
/// Note: the include-flags are named `include_stdout` / `include_stderr` rather
/// than `stdout` / `stderr` because the latter are reserved macros from <cstdio>
/// on some platforms (notably MSVC, where `stdout` expands to a function call).
/// There is deliberately no `follow` flag here: the snapshot/stream split is
/// encoded in the method names — `logs()` is always a snapshot, `follow_logs()`
/// always streams.
struct LogOptions {
    bool include_stdout = true; ///< include the stdout stream
    bool include_stderr = true; ///< include the stderr stream
    std::string tail = "all";   ///< number of trailing lines, or "all"
    bool timestamps = false;    ///< prefix each line with an RFC3339 timestamp
    /// The container was created with Tty=true → its log stream is raw/unframed;
    /// skip demux. Not a query param: it only selects the decode path.
    bool tty = false;
};

/// Combined stdout / stderr text retrieved from a container.
struct ContainerLogs {
    std::string stdout_data;
    std::string stderr_data;
};

/// Which stream a streamed log chunk came from.
enum class LogSource { Stdout, Stderr };

/// A streaming log consumer. Invoked for each decoded chunk of log output with
/// the source stream and the chunk bytes (the string_view is valid only for the
/// duration of the call — copy if you need to retain it). Return true to keep
/// receiving, false to stop the stream early.
using LogConsumer = std::function<bool(LogSource source, std::string_view data)>;

} // namespace testcontainers
