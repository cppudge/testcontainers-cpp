#pragma once

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <optional>

namespace testcontainers::detail {

/// Run `ioc` until the single pending async operation marks `done` or
/// `timeout` expires. On expiry invoke `cancel` and keep running until the
/// handler fires anyway — it may still reference caller-owned buffers, so it
/// must complete before this returns — then report the expiry as
/// asio::error::timed_out (unless the operation genuinely completed while
/// draining, in which case its real result stands). No timeout -> run to
/// completion.
template <class Cancel>
void run_pending(boost::asio::io_context& ioc,
                 const std::optional<std::chrono::milliseconds>& timeout, const bool& done,
                 boost::system::error_code& ec, Cancel cancel) {
    ioc.restart();
    if (!timeout) {
        ioc.run();
        return;
    }
    ioc.run_for(*timeout);
    if (done) {
        return;
    }
    cancel();
    ioc.run();
    if (!done || ec == boost::asio::error::operation_aborted) {
        ec = boost::asio::error::timed_out;
    }
}

} // namespace testcontainers::detail
