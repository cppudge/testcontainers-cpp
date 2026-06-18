#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/WaitFor.hpp"

// Tests in this file:
//   WaitFor.StdoutMessageFactory - wait_for::stdout_message builds a LogMessage targeting stdout with the given text and times.
//   WaitFor.StderrMessageFactory - wait_for::stderr_message builds a LogMessage targeting stderr.
//   WaitFor.LogFactoryIsEither - wait_for::log builds a LogMessage scanning either stream.
//   WaitFor.SecondsFactory - wait_for::seconds builds a Duration of the right millisecond value.
//   WaitFor.MillisFactory - wait_for::millis builds a Duration of the right millisecond value.
//   WaitFor.Copyable - a WaitFor (and a vector of them) can be copied.
//   WaitFor.VisitDispatches - std::visit dispatches to the active alternative.

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

TEST(WaitFor, Copyable) {
    const WaitFor w = wait_for::stdout_message("ready");
    WaitFor copy = w; // copy-construct
    EXPECT_EQ(std::get<wait::LogMessage>(copy).text, "ready");

    std::vector<WaitFor> waits;
    waits.push_back(wait_for::log("a"));
    waits.push_back(wait_for::seconds(1));
    waits.push_back(wait::None{});
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
