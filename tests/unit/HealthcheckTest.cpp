#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "testcontainers/Healthcheck.hpp"

// Tests in this file:
//   Healthcheck.CmdShellFactory - cmd_shell builds a {"CMD-SHELL", cmd} test array with no durations set.
//   Healthcheck.CmdFactory - cmd builds a {"CMD", arg0, arg1, ...} test array.
//   Healthcheck.NoneFactory - none builds a {"NONE"} test array.
//   Healthcheck.SettersSetFields - with_interval/with_timeout/with_retries/with_start_period populate the matching optionals.
//   Healthcheck.SettersChainOnRvalue - the rvalue-qualified setters chain on a temporary.

using namespace testcontainers;
using namespace std::chrono_literals;

TEST(Healthcheck, CmdShellFactory) {
    const Healthcheck hc = Healthcheck::cmd_shell("curl -f http://localhost/");
    EXPECT_EQ(hc.test(), (std::vector<std::string>{"CMD-SHELL", "curl -f http://localhost/"}));
    EXPECT_FALSE(hc.interval().has_value());
    EXPECT_FALSE(hc.timeout().has_value());
    EXPECT_FALSE(hc.start_period().has_value());
    EXPECT_FALSE(hc.retries().has_value());
}

TEST(Healthcheck, CmdFactory) {
    const Healthcheck hc = Healthcheck::cmd({"pg_isready", "-U", "postgres"});
    EXPECT_EQ(hc.test(), (std::vector<std::string>{"CMD", "pg_isready", "-U", "postgres"}));
}

TEST(Healthcheck, NoneFactory) {
    const Healthcheck hc = Healthcheck::none();
    EXPECT_EQ(hc.test(), (std::vector<std::string>{"NONE"}));
}

TEST(Healthcheck, SettersSetFields) {
    Healthcheck hc = Healthcheck::cmd_shell("exit 0");
    hc.with_interval(500ms).with_timeout(1s).with_retries(3).with_start_period(2s);

    ASSERT_TRUE(hc.interval().has_value());
    EXPECT_EQ(*hc.interval(), std::chrono::nanoseconds(500ms));
    ASSERT_TRUE(hc.timeout().has_value());
    EXPECT_EQ(*hc.timeout(), std::chrono::nanoseconds(1s));
    ASSERT_TRUE(hc.start_period().has_value());
    EXPECT_EQ(*hc.start_period(), std::chrono::nanoseconds(2s));
    ASSERT_TRUE(hc.retries().has_value());
    EXPECT_EQ(*hc.retries(), 3);
}

TEST(Healthcheck, SettersChainOnRvalue) {
    const Healthcheck hc = Healthcheck::cmd_shell("exit 0").with_interval(250ms).with_retries(5);
    ASSERT_TRUE(hc.interval().has_value());
    EXPECT_EQ(*hc.interval(), std::chrono::nanoseconds(250ms));
    ASSERT_TRUE(hc.retries().has_value());
    EXPECT_EQ(*hc.retries(), 5);
}
