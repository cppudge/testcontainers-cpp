#pragma once

#include <chrono>

namespace testcontainers::detail {

/// `at + delta` with saturation at `time_point::max()`. User-sized budgets
/// ("wait forever" spelled as milliseconds::max(), hours(300000), ...) must
/// clamp to the far future: the plain `+` is signed-integer overflow (UB),
/// and the typical wrap lands the deadline in the PAST, timing a healthy
/// wait out instantly. The overflow window is stdlib-dependent — MSVC's
/// steady_clock rep ticks at 100ns while libstdc++'s at 1ns — so the same
/// budget can be green on one toolchain and wrapped on the other; the
/// comparison therefore happens in milliseconds, BEFORE any conversion to
/// the clock's finer rep. A non-positive delta yields `at` unchanged (an
/// already-due deadline).
inline std::chrono::steady_clock::time_point saturated_add(std::chrono::steady_clock::time_point at,
                                                           std::chrono::milliseconds delta) {
    using clock = std::chrono::steady_clock;
    if (delta <= std::chrono::milliseconds::zero()) {
        return at;
    }
    // Truncation makes headroom conservative (never larger than the real
    // room), so `delta < headroom` guarantees the converted add fits.
    const auto headroom =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::time_point::max() - at);
    return delta >= headroom ? clock::time_point::max() : at + delta;
}

} // namespace testcontainers::detail
