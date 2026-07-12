#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>

#include "testcontainers/Container.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/TtySize.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   Tty.LogsAreRawNotFramed - a Tty=true container's logs() returns the raw/unframed stream (stdout contains the text, stderr empty, no stray multiplex header).
//   Tty.LogWaitWorksOnTtyContainer - wait_for::log succeeds on a Tty=true container, proving the log-wait reads the raw stream instead of garbled multiplex bytes.
//   Tty.FollowLogsDeliversRaw - follow_logs on a Tty=true container delivers raw chunks (as Stdout) containing the expected text.
//   Tty.ResizeTtyChangesWindowSize - resize_tty() reaches the container's pseudo-TTY: a loop printing `stty size` starts reporting the new rows x columns.

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available.
class Tty : public tcit::LinuxEngineTest {};

TEST_F(Tty, LogsAreRawNotFramed) {
    // A TTY container emits a raw, unframed log stream (no 8-byte multiplex
    // header). The text must survive verbatim in stdout_data, with no separate
    // stderr channel.
    Container c = GenericImage("alpine", "3.20")
                      .with_tty()
                      .with_cmd({"sh", "-c", "echo tty-line; sleep 5"})
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("tty-line"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_TRUE(logs.stderr_data.empty());
    // The first byte of a raw TTY stream is the actual output (here 't'), not a
    // multiplex stream-type header byte (0x00/0x01/0x02).
    ASSERT_FALSE(logs.stdout_data.empty());
    const unsigned char first = static_cast<unsigned char>(logs.stdout_data.front());
    EXPECT_GE(first, 0x20u) << "first byte 0x" << std::hex << static_cast<int>(first)
                            << " looks like a multiplex header, not raw output";
}

TEST_F(Tty, LogWaitWorksOnTtyContainer) {
    // wait_for::log polls logs() under the hood; on a TTY container that means
    // reading the raw stream. If the wait honored the TTY flag, start() returns
    // without throwing/timing out.
    EXPECT_NO_THROW({
        Container c = GenericImage("alpine", "3.20")
                          .with_tty()
                          .with_cmd({"sh", "-c", "echo READY; sleep 5"})
                          .with_wait(wait_for::log("READY"))
                          .start();
        (void)c;
    });
}

TEST_F(Tty, FollowLogsDeliversRaw) {
    Container c = GenericImage("alpine", "3.20")
                      .with_tty()
                      .with_cmd({"sh", "-c", "echo follow-tty; echo second-line; sleep 5"})
                      .start();

    std::string collected;
    // Returning false once the text is seen closes the connection promptly so the
    // call does not block for the container's full lifetime.
    c.follow_logs(
        [&](LogSource source, std::string_view data) {
            // A TTY stream has no separate stderr channel — everything is Stdout.
            EXPECT_EQ(source, LogSource::Stdout);
            collected.append(data);
            return collected.find("follow-tty") == std::string::npos; // stop once seen
        },
        LogOptions{});

    EXPECT_NE(collected.find("follow-tty"), std::string::npos) << "collected: " << collected;
}

TEST_F(Tty, ResizeTtyChangesWindowSize) {
    // The loop prints the pseudo-TTY's size a few times a second; after
    // resize_tty the kernel delivers SIGWINCH and subsequent `stty size`
    // calls report the new dimensions. The bracketed marker makes the match
    // exact, and the asymmetric 44x144 cannot be a platform default.
    Container c = GenericImage("alpine", "3.20")
                      .with_tty()
                      .with_cmd({"sh", "-c",
                                 "while true; do echo \"size=[$(stty size)]\"; "
                                 "sleep 0.2; done"})
                      .start();

    c.resize_tty(TtySize{44, 144});

    // Follow the raw TTY log stream (deadline-bounded) until the new size
    // shows up; a resize that never lands ends the follow at the deadline
    // with the marker missing.
    DockerClient client = DockerClient::from_environment();
    LogOptions opts;
    opts.tty = true;
    std::string collected;
    (void)client.follow_logs(
        c.id(), opts,
        [&](LogSource, std::string_view data) {
            collected.append(data);
            return collected.find("size=[44 144]") == std::string::npos; // stop once seen
        },
        std::chrono::steady_clock::now() + std::chrono::seconds(30));

    EXPECT_NE(collected.find("size=[44 144]"), std::string::npos) << "collected: " << collected;
}
