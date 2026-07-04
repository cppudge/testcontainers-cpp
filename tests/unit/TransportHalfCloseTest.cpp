#include <gtest/gtest.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <string>
#include <thread>

#include "docker/Transport.hpp"
#include "testcontainers/docker/DockerHost.hpp"
#include "testcontainers/docker/Timeouts.hpp"

// Tests in this file (Windows-only; in-process named-pipe servers, no Docker
// daemon — the zero-length-message half-close of the named-pipe transport):
//   NamedPipeHalfClose.DeliversEofOnMessageModePipe - on a MESSAGE-mode pipe (what a real daemon serves), shutdown_send() delivers a zero-byte read (EOF) to the server, and the client can still READ the server's reply afterwards — the exec-stdin shape (write stdin, half-close, read output).
//   NamedPipeHalfClose.ByteModePipeReportsNoHalfClose - on a BYTE-mode pipe a zero-byte write is invisible, so supports_half_close() is false and shutdown_send() is a harmless no-op (the loud-throw guard stays reachable).

namespace {

using namespace std::chrono_literals;

using testcontainers::DockerHost;
using testcontainers::docker::TransportTimeouts;

std::string pipe_name(const char* tag) {
    return std::string(R"(\\.\pipe\tc-halfclose-)") + tag + "-" +
           std::to_string(::GetCurrentProcessId());
}

/// The npipe:// form DockerHost::parse takes (forward slashes; the transport
/// converts back).
DockerHost pipe_host(const char* tag) {
    return DockerHost::parse("npipe:////./pipe/tc-halfclose-" + std::string(tag) + "-" +
                             std::to_string(::GetCurrentProcessId()));
}

} // namespace

TEST(NamedPipeHalfClose, DeliversEofOnMessageModePipe) {
    // A message-mode pipe server — the mode moby creates the daemon pipe in.
    // Read sequence mirrors the daemon side of exec-stdin: read the payload
    // message, then observe the zero-length message (a 0-byte read = EOF),
    // then still WRITE a reply the client must be able to read — proving the
    // half-close closed only the client->server direction.
    const std::string name = pipe_name("msg");
    const HANDLE pipe =
        ::CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
                           PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                           /*instances*/ 1, /*out buf*/ 4096, /*in buf*/ 4096,
                           /*default timeout*/ 0, /*security*/ nullptr);
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE) << "CreateNamedPipeA: " << ::GetLastError();

    std::promise<std::string> payload_promise;
    std::promise<DWORD> eof_bytes_promise;
    std::promise<bool> eof_ok_promise;
    std::thread server([&] {
        ::ConnectNamedPipe(pipe, nullptr); // blocks until the client connects

        char buf[256] = {};
        DWORD n = 0;
        const BOOL read_ok = ::ReadFile(pipe, buf, sizeof(buf), &n, nullptr);
        payload_promise.set_value(read_ok ? std::string(buf, n) : std::string("<read failed>"));

        // The half-close: a zero-length message arrives as a SUCCESSFUL
        // zero-byte read (this is what go-winio's reader maps to io.EOF).
        DWORD eof_n = 0;
        const BOOL eof_ok = ::ReadFile(pipe, buf, sizeof(buf), &eof_n, nullptr);
        eof_ok_promise.set_value(eof_ok != 0);
        eof_bytes_promise.set_value(eof_n);

        // The reply the client must still be able to read.
        const std::string reply = "world";
        DWORD written = 0;
        ::WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr);
        ::FlushFileBuffers(pipe);
    });

    TransportTimeouts timeouts;
    timeouts.io = 5s; // a broken half-close must fail the test, not hang it
    const auto transport = testcontainers::docker::connect(pipe_host("msg"), timeouts);

    EXPECT_TRUE(transport->supports_half_close());

    boost::system::error_code ec;
    const std::string payload = "hello";
    ASSERT_EQ(transport->write_some(payload.data(), payload.size(), ec), payload.size());
    ASSERT_FALSE(ec) << ec.message();

    transport->shutdown_send();

    // The read side must still work after the half-close.
    char reply_buf[16] = {};
    const std::size_t reply_n = transport->read_some(reply_buf, sizeof(reply_buf), ec);
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(std::string(reply_buf, reply_n), "world");

    // Close the client BEFORE harvesting the server-side observations: if the
    // EOF message never arrived (a shutdown_send regression), the server is
    // still blocked in its ReadFile — closing our end unblocks it, so the
    // futures resolve and the test FAILS on the assertions instead of hanging
    // forever in get().
    transport->close();

    EXPECT_EQ(payload_promise.get_future().get(), "hello");
    EXPECT_TRUE(eof_ok_promise.get_future().get())
        << "the zero-length message must complete the read successfully";
    EXPECT_EQ(eof_bytes_promise.get_future().get(), 0u)
        << "the EOF signal is a zero-byte read, not data";

    server.join();
    ::CloseHandle(pipe);
}

TEST(NamedPipeHalfClose, ByteModePipeReportsNoHalfClose) {
    // On a BYTE-mode pipe a zero-byte WriteFile is invisible to the reader —
    // there is no EOF to deliver. The transport must say so (the exec-stdin
    // guard then throws loudly instead of hanging the in-container reader),
    // and shutdown_send() must be a harmless no-op.
    const std::string name = pipe_name("byte");
    const HANDLE pipe =
        ::CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           /*instances*/ 1, /*out buf*/ 4096, /*in buf*/ 4096,
                           /*default timeout*/ 0, /*security*/ nullptr);
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE) << "CreateNamedPipeA: " << ::GetLastError();

    std::promise<void> stop;
    std::thread server([&] {
        ::ConnectNamedPipe(pipe, nullptr);
        stop.get_future().wait();
    });

    TransportTimeouts timeouts;
    timeouts.io = 5s;
    const auto transport = testcontainers::docker::connect(pipe_host("byte"), timeouts);

    EXPECT_FALSE(transport->supports_half_close());
    transport->shutdown_send(); // must not throw, hang, or write anything

    transport->close();
    stop.set_value();
    server.join();
    ::CloseHandle(pipe);
}

#endif // _WIN32
