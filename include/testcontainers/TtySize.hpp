#pragma once

#include <cstdint>

namespace testcontainers {

/// A pseudo-TTY's dimensions in character cells. Docker's wire order is
/// [height, width] — rows first — and this struct follows it.
struct TtySize {
    std::uint16_t height = 0; ///< rows
    std::uint16_t width = 0;  ///< columns
};

} // namespace testcontainers
