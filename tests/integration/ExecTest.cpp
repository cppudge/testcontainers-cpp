#include <gtest/gtest.h>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/DockerHost.hpp"
#include "testcontainers/docker/Logs.hpp"

#include "CapMask.hpp"
#include "EngineGuard.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// Exec suite, Windows mode for the WindowsExec mirror):
//   Exec.CapturesStdoutAndZeroExit - exec'ing `echo` in a running container captures the stdout text and reports exit code 0.
//   Exec.PropagatesNonZeroExit - exec'ing a command that exits 5 reports exit code 5.
//   Exec.PassesEnv - ExecOptions.env entries are visible to the command (echo $FOO -> "bar").
//   Exec.UsesWorkingDir - ExecOptions.working_dir sets the command's cwd (pwd -> "/tmp").
//   Exec.RunsAsUser - ExecOptions.user runs the command as that uid (id -u -> "0").
//   Exec.TtyCapturesRawStdout - tty=true returns raw output in stdout_data with stderr_data empty.
//   Exec.StreamsOutputIncrementally - the streaming overload delivers chunks to the consumer and reports the exit code.
//   Exec.StreamingStopsWhenConsumerReturnsFalse - returning false from the consumer stops the stream early.
//   Exec.FeedsStdin - ExecOptions.stdin_data is piped to the command's stdin and read back (cat -> "ping"); runs on unix socket, TCP, AND the Windows named pipe (zero-length-message EOF); skipped on TLS (no half-close).
//   Exec.StdinThrowsOnNonHalfClosableTransport - on the TLS transport, exec with stdin_data throws DockerError up front instead of hanging the reader; skipped on transports that half-close.
//   Exec.PrivilegedExecExpandsCapabilities - ExecOptions.privileged grants the exec process a strict superset of the unprivileged exec's effective capabilities.
//   WindowsExec.PropagatesNonZeroExit - cmd `exit 5` in a Windows container reports exit code 5 (the basic stdout capture is covered by WindowsContainer.ExecRunsInRunningContainer).
//   WindowsExec.PassesEnv - ExecOptions.env entries are visible to cmd (`echo %FOO%` -> "bar").
//   WindowsExec.UsesWorkingDir - ExecOptions.working_dir sets the command's cwd (`cd` -> "C:\Windows").
//   WindowsExec.RunsAsUser - ExecOptions.user runs the command as that Windows account (%USERNAME% -> "ContainerAdministrator").
//   WindowsExec.TtyCapturesRawStdout - tty=true against a Windows container returns raw output in stdout_data with stderr_data empty.
//   WindowsExec.StreamsOutputIncrementally - the streaming overload works against a Windows daemon.
//   WindowsExec.StreamingStopsWhenConsumerReturnsFalse - returning false stops a multi-chunk Windows exec stream early without hanging.
//   WindowsExec.FeedsStdin - stdin_data is piped into `cmd /q`, which executes the scripted commands (Windows containers + the half-closable transports; skipped on TLS).
//   (No Windows mirror for StdinThrowsOnNonHalfClosableTransport: the up-front
//   guard is client-side and engine-independent.)

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available.
class Exec : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(Exec, CapturesStdoutAndZeroExit) {
    // A long-running container so the exec has something to attach to; no wait
    // strategy is needed because `sleep` produces no readiness signal.
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const ExecResult res = c.exec({"echo", "hello-exec"});
    EXPECT_NE(res.stdout_data.find("hello-exec"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(Exec, PropagatesNonZeroExit) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const ExecResult res = c.exec({"sh", "-c", "exit 5"});
    EXPECT_EQ(res.exit_code, 5);
}

TEST_F(Exec, PassesEnv) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    ExecOptions opts;
    opts.env = {"FOO=bar"};
    const ExecResult res = c.exec({"sh", "-c", "echo $FOO"}, opts);
    EXPECT_NE(res.stdout_data.find("bar"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(Exec, UsesWorkingDir) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    ExecOptions opts;
    opts.working_dir = "/tmp";
    const ExecResult res = c.exec({"pwd"}, opts);
    EXPECT_NE(res.stdout_data.find("/tmp"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(Exec, RunsAsUser) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    ExecOptions opts;
    opts.user = "0"; // root
    const ExecResult res = c.exec({"id", "-u"}, opts);
    EXPECT_NE(res.stdout_data.find("0"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(Exec, TtyCapturesRawStdout) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    ExecOptions opts;
    opts.tty = true;
    const ExecResult res = c.exec({"echo", "tty-hello"}, opts);
    // A TTY stream is raw/unframed: the text lands in stdout_data verbatim and
    // stderr_data is never populated (there is no separate stderr channel).
    EXPECT_NE(res.stdout_data.find("tty-hello"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_TRUE(res.stderr_data.empty());
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(Exec, StreamsOutputIncrementally) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    std::string collected;
    const ExecResult res = c.exec({"echo", "stream-me"}, ExecOptions{},
                                  [&](LogSource, std::string_view data) {
                                      collected.append(data);
                                      return true; // keep receiving
                                  });
    EXPECT_NE(collected.find("stream-me"), std::string::npos) << "collected: " << collected;
    EXPECT_EQ(res.exit_code, 0);
    // The streaming overload delivers output to the consumer, not the result.
    EXPECT_TRUE(res.stdout_data.empty());
    EXPECT_TRUE(res.stderr_data.empty());
}

TEST_F(Exec, StreamingStopsWhenConsumerReturnsFalse) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    // Emit several lines; the consumer returns false after the first chunk, which
    // stops the stream early (the connection is closed). We should not hang.
    int chunks = 0;
    const ExecResult res =
        c.exec({"sh", "-c", "for i in 1 2 3 4 5; do echo line$i; sleep 0.2; done"},
               ExecOptions{}, [&](LogSource, std::string_view) {
                   ++chunks;
                   return false; // stop immediately
               });
    EXPECT_GE(chunks, 1);
    // Exit code is still read from the exec inspect (it may be 0 or non-zero
    // depending on how far the command got before we closed the stream); the key
    // assertion is that returning false stopped streaming without hanging.
    (void)res;
}

TEST_F(Exec, FeedsStdin) {
    // Feeding stdin relies on half-closing the send side so the in-container
    // reader sees EOF. Unix sockets and TCP shutdown(send); the Windows named
    // pipe sends a zero-length message (the daemon pipe is message-mode; this
    // is `docker exec -i`'s mechanism). Only TLS cannot signal EOF (SSL has no
    // half-close) — skip there rather than hang.
    const DockerScheme scheme = DockerClient::from_environment().host().scheme();
    if (scheme == DockerScheme::Https) {
        GTEST_SKIP(); // exec-stdin needs a half-closable transport; TLS cannot signal EOF
    }

    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    // `cat` echoes stdin to stdout and exits on EOF. The half-close after writing
    // the input is what lets cat see EOF and terminate.
    ExecOptions opts;
    opts.stdin_data = "ping\n";
    const ExecResult res = c.exec({"cat"}, opts);
    EXPECT_NE(res.stdout_data.find("ping"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(Exec, StdinThrowsOnNonHalfClosableTransport) {
    // The inverse of FeedsStdin: on a transport whose shutdown_send() is a no-op
    // (TLS; a byte-mode named pipe) exec must fail loudly instead of leaving the
    // in-container reader waiting forever for an EOF that never comes. A real
    // daemon's named pipe is message-mode and half-closes, so this only fires
    // on TLS.
    const DockerScheme scheme = DockerClient::from_environment().host().scheme();
    if (scheme != DockerScheme::Https) {
        GTEST_SKIP(); // this transport half-closes; the guard only fires on TLS
    }

    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    ExecOptions opts;
    opts.stdin_data = "ping\n";
    EXPECT_THROW((void)c.exec({"cat"}, opts), DockerError);
}

TEST_F(Exec, PrivilegedExecExpandsCapabilities) {
    // The container itself is NOT privileged; only the exec asks for privilege.
    // Assumes a ROOTFUL daemon (CI and Docker Desktop are): a rootless daemon
    // cannot grant extra capabilities, making privileged == plain below.
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const ExecResult plain = c.exec({"sh", "-c", "grep CapEff /proc/self/status"});
    ASSERT_EQ(plain.exit_code, 0) << "stderr: " << plain.stderr_data;
    const std::uint64_t plain_caps = tcit::cap_mask_after(plain.stdout_data, "CapEff:");
    ASSERT_NE(plain_caps, 0u) << "no CapEff line; stdout: " << plain.stdout_data;

    ExecOptions opts;
    opts.privileged = true;
    const ExecResult priv = c.exec({"sh", "-c", "grep CapEff /proc/self/status"}, opts);
    ASSERT_EQ(priv.exit_code, 0) << "stderr: " << priv.stderr_data;
    const std::uint64_t priv_caps = tcit::cap_mask_after(priv.stdout_data, "CapEff:");

    // Privileged = a strict superset of the default exec's capabilities.
    EXPECT_EQ(priv_caps & plain_caps, plain_caps)
        << "privileged caps lost some default bits: " << priv.stdout_data;
    EXPECT_NE(priv_caps, plain_caps)
        << "privileged exec gained nothing: " << priv.stdout_data;
}

// The Windows mirror: every exec surface exercised above, but against a Windows
// daemon and cmd.exe. On CI this rides the DIRECT dockerd named pipe — the
// transport path with the go-winio zero-length-message EOF semantics.
class WindowsExec : public tcit::WindowsEngineTest {
protected:
    /// A running nanoserver container to exec into.
    testcontainers::Container start_keep_alive() {
        return nanoserver().with_cmd(keep_alive_cmd()).start();
    }
};

TEST_F(WindowsExec, PropagatesNonZeroExit) {
    Container c = start_keep_alive();

    const ExecResult res = c.exec({"cmd", "/c", "exit 5"});
    EXPECT_EQ(res.exit_code, 5);
}

TEST_F(WindowsExec, PassesEnv) {
    Container c = start_keep_alive();

    ExecOptions opts;
    opts.env = {"FOO=bar"};
    const ExecResult res = c.exec({"cmd", "/c", "echo %FOO%"}, opts);
    EXPECT_NE(res.stdout_data.find("bar"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(WindowsExec, UsesWorkingDir) {
    Container c = start_keep_alive();

    ExecOptions opts;
    opts.working_dir = "C:\\Windows";
    // A bare `cd` prints the current directory.
    const ExecResult res = c.exec({"cmd", "/c", "cd"}, opts);
    EXPECT_NE(res.stdout_data.find("C:\\Windows"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(WindowsExec, RunsAsUser) {
    Container c = start_keep_alive();

    // Windows containers take account names, not uids; ContainerAdministrator
    // exists in every Windows base image.
    ExecOptions opts;
    opts.user = "ContainerAdministrator";
    const ExecResult res = c.exec({"cmd", "/c", "echo %USERNAME%"}, opts);
    EXPECT_NE(res.stdout_data.find("ContainerAdministrator"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(WindowsExec, TtyCapturesRawStdout) {
    Container c = start_keep_alive();

    ExecOptions opts;
    opts.tty = true;
    const ExecResult res = c.exec({"cmd", "/c", "echo tty-hello-win"}, opts);
    // A TTY stream is raw/unframed: the text lands in stdout_data verbatim and
    // stderr_data is never populated (there is no separate stderr channel).
    EXPECT_NE(res.stdout_data.find("tty-hello-win"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_TRUE(res.stderr_data.empty());
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(WindowsExec, StreamsOutputIncrementally) {
    Container c = start_keep_alive();

    std::string collected;
    const ExecResult res = c.exec({"cmd", "/c", "echo stream-me-win"}, ExecOptions{},
                                  [&](LogSource, std::string_view data) {
                                      collected.append(data);
                                      return true; // keep receiving
                                  });
    EXPECT_NE(collected.find("stream-me-win"), std::string::npos) << "collected: " << collected;
    EXPECT_EQ(res.exit_code, 0);
    // The streaming overload delivers output to the consumer, not the result.
    EXPECT_TRUE(res.stdout_data.empty());
    EXPECT_TRUE(res.stderr_data.empty());
}

TEST_F(WindowsExec, StreamingStopsWhenConsumerReturnsFalse) {
    Container c = start_keep_alive();

    // Emit several lines with ~1s pauses (`ping -n 2` sleeps one second); the
    // consumer returns false after the first chunk, which stops the stream
    // early (the connection is closed). We should not hang.
    int chunks = 0;
    const ExecResult res = c.exec(
        {"cmd", "/c", "for /L %i in (1,1,5) do (echo line%i & ping -n 2 127.0.0.1 >nul)"},
        ExecOptions{}, [&](LogSource, std::string_view) {
            ++chunks;
            return false; // stop immediately
        });
    EXPECT_GE(chunks, 1);
    // Exit code is still read from the exec inspect (it may be 0 or non-zero
    // depending on how far the command got before we closed the stream); the key
    // assertion is that returning false stopped streaming without hanging.
    (void)res;
}

TEST_F(WindowsExec, FeedsStdin) {
    // Same transport rule as Exec.FeedsStdin: stdin needs a half-closable
    // transport; only TLS cannot signal EOF.
    const DockerScheme scheme = DockerClient::from_environment().host().scheme();
    if (scheme == DockerScheme::Https) {
        GTEST_SKIP(); // exec-stdin needs a half-closable transport; TLS cannot signal EOF
    }

    Container c = start_keep_alive();

    // nanoserver has no `cat`, but cmd.exe itself executes commands read from
    // stdin: pipe in a script that echoes a marker and exits. `/q` suppresses
    // command echoing so the output is just the marker (plus cmd's banner).
    ExecOptions opts;
    opts.stdin_data = "echo stdin-ping-win\r\nexit\r\n";
    const ExecResult res = c.exec({"cmd", "/q"}, opts);
    EXPECT_NE(res.stdout_data.find("stdin-ping-win"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}
