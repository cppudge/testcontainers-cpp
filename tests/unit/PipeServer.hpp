#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <utility>

#include "TestSupport.hpp"

namespace tcunit {

/// A one-instance local named-pipe server: creates the pipe (message or byte
/// mode, 4 KiB buffers), accepts ONE client on its own thread, runs `session`
/// on the connected pipe, then HOLDS the pipe open — the shape of a silent /
/// wedged daemon — until destruction. The destructor completes a
/// never-arrived ConnectNamedPipe with a throwaway client (the pipe mirror of
/// the TCP accept-wake in LoopbackServer.hpp), joins, and closes the handle.
///
/// Check valid() (CreateNamedPipe can fail) before connecting a client, and
/// close the client's transport BEFORE the server goes out of scope when the
/// session could still be blocked reading it.
class PipeServer {
public:
    enum class Mode { Byte, Message };
    using Session = std::function<void(HANDLE pipe)>;

    PipeServer(const std::string& tag, Mode mode, Session session = {})
        : tag_(tag), name_(pipe_name(tag)),
          pipe_(::CreateNamedPipeA(name_.c_str(), PIPE_ACCESS_DUPLEX,
                                   (mode == Mode::Message
                                        ? (PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE)
                                        : (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE)) |
                                       PIPE_WAIT,
                                   /*instances*/ 1, /*out buf*/ 4096, /*in buf*/ 4096,
                                   /*default timeout*/ 0, /*security*/ nullptr)) {
        if (pipe_ == INVALID_HANDLE_VALUE) {
            return; // valid() reports it; the test asserts before connecting
        }
        thread_ = std::thread([this, session = std::move(session)] {
            ::ConnectNamedPipe(pipe_, nullptr); // blocks until a client connects
            if (stopping_) {
                return; // the destructor's throwaway client, not a real one
            }
            if (session) {
                session(pipe_);
            }
            stop_.get_future().wait(); // hold the pipe open, silently
        });
    }

    ~PipeServer() {
        try {
            if (thread_.joinable()) {
                stopping_ = true;
                stop_.set_value();
                // Complete a still-pending ConnectNamedPipe deterministically;
                // if a client already connected this fails with
                // ERROR_PIPE_BUSY (one instance), which is exactly as
                // harmless.
                const HANDLE poke = ::CreateFileA(name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                                  nullptr, OPEN_EXISTING, 0, nullptr);
                if (poke != INVALID_HANDLE_VALUE) {
                    ::CloseHandle(poke);
                }
                thread_.join();
            }
        } catch (...) {
            // Best-effort: a destructor must never throw (join only throws
            // when the thread is already unjoinable).
        }
        if (pipe_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(pipe_);
        }
    }

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    bool valid() const noexcept { return pipe_ != INVALID_HANDLE_VALUE; }

    /// The npipe:// DockerHost a client connects with.
    testcontainers::DockerHost host() const { return pipe_host(tag_); }

private:
    std::string tag_;
    std::string name_;
    HANDLE pipe_;
    std::atomic<bool> stopping_{false};
    std::promise<void> stop_;
    std::thread thread_;
};

} // namespace tcunit

#endif // _WIN32
