#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "WaitStrategies.hpp"
#include "testcontainers/WaitFor.hpp"

// Tests in this file:
//   WaitFor.StdoutMessageFactory - wait_for::stdout_message builds a LogMessage targeting stdout with the given text and times.
//   WaitFor.StderrMessageFactory - wait_for::stderr_message builds a LogMessage targeting stderr.
//   WaitFor.LogFactoryIsEither - wait_for::log builds a LogMessage scanning either stream.
//   WaitFor.SecondsFactory - wait_for::seconds builds a Duration of the right millisecond value.
//   WaitFor.MillisFactory - wait_for::millis builds a Duration of the right millisecond value.
//   WaitFor.ExitFactory - wait_for::exit builds an Exit with no required code.
//   WaitFor.ExitCodeFactory - wait_for::exit_code builds an Exit pinned to the given code.
//   WaitFor.HealthyFactory - wait_for::healthy builds a Healthcheck alternative.
//   WaitFor.HttpFactory - wait_for::http builds an Http with the given path, port, and status (and a default poll interval).
//   WaitFor.HttpFactoryDefaultStatus - wait_for::http defaults the expected status to 200.
//   WaitFor.ListeningPortFactory - wait_for::listening_port builds a Port with the given port (and a default poll interval).
//   WaitFor.Copyable - a WaitFor (and a vector of them) can be copied.
//   WaitFor.VisitDispatches - std::visit dispatches to the active alternative.
//   WaitFor.CountOccurrencesBasics - count_occurrences counts disjoint matches, including at the ends, and 0 for no match.
//   WaitFor.CountOccurrencesNonOverlapping - overlapping candidates count once per consumed match ("aaaa"/"aa" -> 2).
//   WaitFor.CountOccurrencesEmptyNeedleIsZero - an empty needle yields 0 (never "instantly satisfied").
//   WaitFor.OccurrenceCounterMatchesAcrossChunkBoundaries - the streaming counter finds a needle split at arbitrary chunk boundaries (single-byte feeds included).
//   WaitFor.OccurrenceCounterAgreesWithSnapshotCount - for any chunking of the same text, the streaming count equals count_occurrences on the whole.
//   WaitFor.OccurrenceCounterDoesNotRescanConsumedBytes - bytes consumed by a match cannot seed a new match with later chunks ("baa"+"a"/"aa" stays 1, like the snapshot count of "baaa").
//   WaitFor.OccurrenceCounterEmptyNeedleCountsNothing - parity with count_occurrences.
//   WaitFor.OccurrenceCounterMatchesAfterPrefixTrim - a match still lands after the internal >64KiB consumed-prefix compaction.

using namespace testcontainers;

TEST(WaitFor, StdoutMessageFactory) {
    const WaitFor w = wait_for::stdout_message("ready", 3);
    ASSERT_TRUE(std::holds_alternative<wait::LogMessage>(w));
    const auto& m = std::get<wait::LogMessage>(w);
    EXPECT_EQ(m.text, "ready");
    EXPECT_EQ(m.times, 3);
    EXPECT_EQ(m.source, wait::LogMessage::Source::Stdout);
}

TEST(WaitFor, StderrMessageFactory) {
    const WaitFor w = wait_for::stderr_message("boom");
    ASSERT_TRUE(std::holds_alternative<wait::LogMessage>(w));
    const auto& m = std::get<wait::LogMessage>(w);
    EXPECT_EQ(m.text, "boom");
    EXPECT_EQ(m.times, 1); // default
    EXPECT_EQ(m.source, wait::LogMessage::Source::Stderr);
}

TEST(WaitFor, LogFactoryIsEither) {
    const WaitFor w = wait_for::log("up");
    ASSERT_TRUE(std::holds_alternative<wait::LogMessage>(w));
    EXPECT_EQ(std::get<wait::LogMessage>(w).source, wait::LogMessage::Source::Either);
}

TEST(WaitFor, SecondsFactory) {
    const WaitFor w = wait_for::seconds(2);
    ASSERT_TRUE(std::holds_alternative<wait::Duration>(w));
    EXPECT_EQ(std::get<wait::Duration>(w).value, std::chrono::milliseconds(2000));
}

TEST(WaitFor, MillisFactory) {
    const WaitFor w = wait_for::millis(250);
    ASSERT_TRUE(std::holds_alternative<wait::Duration>(w));
    EXPECT_EQ(std::get<wait::Duration>(w).value, std::chrono::milliseconds(250));
}

TEST(WaitFor, ExitFactory) {
    const WaitFor w = wait_for::exit();
    ASSERT_TRUE(std::holds_alternative<wait::Exit>(w));
    EXPECT_FALSE(std::get<wait::Exit>(w).code.has_value());
}

TEST(WaitFor, ExitCodeFactory) {
    const WaitFor w = wait_for::exit_code(7);
    ASSERT_TRUE(std::holds_alternative<wait::Exit>(w));
    const auto& e = std::get<wait::Exit>(w);
    ASSERT_TRUE(e.code.has_value());
    EXPECT_EQ(*e.code, 7);
}

TEST(WaitFor, HealthyFactory) {
    const WaitFor w = wait_for::healthy();
    EXPECT_TRUE(std::holds_alternative<wait::Healthcheck>(w));
}

TEST(WaitFor, HttpFactory) {
    const WaitFor w = wait_for::http("/health", tcp(8080), 204);
    ASSERT_TRUE(std::holds_alternative<wait::Http>(w));
    const auto& h = std::get<wait::Http>(w);
    EXPECT_EQ(h.path, "/health");
    EXPECT_EQ(h.port, tcp(8080));
    EXPECT_EQ(h.expected_status, 204);
    EXPECT_EQ(h.poll_interval, std::chrono::milliseconds(200)); // default
}

TEST(WaitFor, HttpFactoryDefaultStatus) {
    const WaitFor w = wait_for::http("/", tcp(80));
    ASSERT_TRUE(std::holds_alternative<wait::Http>(w));
    EXPECT_EQ(std::get<wait::Http>(w).expected_status, 200);
}

TEST(WaitFor, ListeningPortFactory) {
    const WaitFor w = wait_for::listening_port(tcp(5432));
    ASSERT_TRUE(std::holds_alternative<wait::Port>(w));
    const auto& p = std::get<wait::Port>(w);
    EXPECT_EQ(p.port, tcp(5432));
    EXPECT_EQ(p.poll_interval, std::chrono::milliseconds(200)); // default
}

TEST(WaitFor, Copyable) {
    const WaitFor w = wait_for::stdout_message("ready");
    WaitFor copy = w; // copy-construct
    EXPECT_EQ(std::get<wait::LogMessage>(copy).text, "ready");

    std::vector<WaitFor> waits;
    waits.push_back(wait_for::log("a"));
    waits.push_back(wait_for::seconds(1));
    waits.emplace_back(wait::None{});
    EXPECT_EQ(waits.size(), 3u);
}

TEST(WaitFor, VisitDispatches) {
    auto describe = [](const WaitFor& w) -> std::string {
        return std::visit(
            [](const auto& cond) -> std::string {
                using T = std::decay_t<decltype(cond)>;
                if constexpr (std::is_same_v<T, wait::None>) {
                    return "none";
                } else if constexpr (std::is_same_v<T, wait::LogMessage>) {
                    return "log";
                } else {
                    return "duration";
                }
            },
            w);
    };

    EXPECT_EQ(describe(wait::None{}), "none");
    EXPECT_EQ(describe(wait_for::log("x")), "log");
    EXPECT_EQ(describe(wait_for::seconds(1)), "duration");
}

TEST(WaitFor, CountOccurrencesBasics) {
    using testcontainers::detail::count_occurrences;
    EXPECT_EQ(count_occurrences("Ready to accept connections", "Ready"), 1u);
    EXPECT_EQ(count_occurrences("ready... ready... ready", "ready"), 3u);
    EXPECT_EQ(count_occurrences("no match here", "READY"), 0u);
    EXPECT_EQ(count_occurrences("", "ready"), 0u);
}

TEST(WaitFor, CountOccurrencesNonOverlapping) {
    using testcontainers::detail::count_occurrences;
    // Matches consume their span: "aaaa" holds two disjoint "aa", not three.
    EXPECT_EQ(count_occurrences("aaaa", "aa"), 2u);
    EXPECT_EQ(count_occurrences("aaa", "aa"), 1u);
}

TEST(WaitFor, CountOccurrencesEmptyNeedleIsZero) {
    using testcontainers::detail::count_occurrences;
    // An empty needle must not read as "message already seen".
    EXPECT_EQ(count_occurrences("anything", ""), 0u);
    EXPECT_EQ(count_occurrences("", ""), 0u);
}

TEST(WaitFor, OccurrenceCounterMatchesAcrossChunkBoundaries) {
    using testcontainers::detail::OccurrenceCounter;

    OccurrenceCounter split("Ready!");
    split.feed("almost Rea");
    EXPECT_EQ(split.count(), 0u);
    split.feed("dy! and more");
    EXPECT_EQ(split.count(), 1u);

    OccurrenceCounter byte_by_byte("ready");
    for (const char c : std::string("ready... ready")) {
        byte_by_byte.feed(std::string_view(&c, 1));
    }
    EXPECT_EQ(byte_by_byte.count(), 2u);
}

TEST(WaitFor, OccurrenceCounterAgreesWithSnapshotCount) {
    using testcontainers::detail::count_occurrences;
    using testcontainers::detail::OccurrenceCounter;

    const std::string text = "aabaaa ready aaaa ready-ready aa";
    for (const std::string& needle : {std::string("aa"), std::string("ready"), std::string("a")}) {
        for (std::size_t chunk = 1; chunk <= text.size(); ++chunk) {
            OccurrenceCounter counter(needle);
            for (std::size_t i = 0; i < text.size(); i += chunk) {
                counter.feed(std::string_view(text).substr(i, chunk));
            }
            EXPECT_EQ(counter.count(), count_occurrences(text, needle))
                << "needle '" << needle << "', chunk size " << chunk;
        }
    }
}

TEST(WaitFor, OccurrenceCounterDoesNotRescanConsumedBytes) {
    using testcontainers::detail::OccurrenceCounter;

    // "baa" + "a" is "baaa": one non-overlapping "aa" (at 1); the trailing 'a'
    // must not pair with the match's consumed second 'a'.
    OccurrenceCounter counter("aa");
    counter.feed("baa");
    EXPECT_EQ(counter.count(), 1u);
    counter.feed("a");
    EXPECT_EQ(counter.count(), 1u);
    counter.feed("a"); // "baaaa": now a second disjoint "aa" exists
    EXPECT_EQ(counter.count(), 2u);
}

TEST(WaitFor, OccurrenceCounterEmptyNeedleCountsNothing) {
    using testcontainers::detail::OccurrenceCounter;

    OccurrenceCounter counter("");
    counter.feed("anything");
    EXPECT_EQ(counter.count(), 0u);
}

TEST(WaitFor, OccurrenceCounterMatchesAfterPrefixTrim) {
    using testcontainers::detail::OccurrenceCounter;

    // Push the consumed prefix past the 64KiB compaction threshold, then check
    // that a boundary-split match still lands (the kept tail must survive the
    // trim intact).
    OccurrenceCounter counter("marker");
    counter.feed(std::string(70000, 'x') + "mar");
    EXPECT_EQ(counter.count(), 0u);
    counter.feed("ker");
    EXPECT_EQ(counter.count(), 1u);
}
