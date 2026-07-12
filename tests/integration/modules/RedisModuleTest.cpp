#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/RedisContainer.hpp"

#include "EngineGuard.hpp"
#include "RedisPing.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   RedisModule.StartsServesAndBuildsDsn - a default RedisContainer starts, answers a raw TCP PING on host()/port(), and connection_string() renders redis://host:port (with /db for a nonzero database index).
//   RedisModule.ExecSetGetRoundTrip - the in-container redis-cli round-trips a SET/GET through container().exec (the no-driver behavioral proof).
//   RedisModule.PasswordIsEnforcedAndWired - with_password really turns auth on (raw PING gets -NOAUTH) while in-container redis-cli authenticates via REDISCLI_AUTH, and the DSN carries :password@.
//   RedisModule.CommandArgsReachTheServer - with_command_args flags reach the running server (config get maxmemory reports the set value).

using namespace testcontainers;
using modules::RedisContainer;
using modules::StartedRedis;

// Requires a Linux-containers daemon; skipped otherwise.
class RedisModule : public tcit::LinuxEngineTest {};

TEST_F(RedisModule, StartsServesAndBuildsDsn) {
    const StartedRedis redis = RedisContainer().start();

    const std::string reply = tcit::redis_ping(redis.host(), redis.port());
    EXPECT_EQ(reply.substr(0, 5), "+PONG");

    const std::string origin = redis.host() + ":" + std::to_string(redis.port());
    EXPECT_EQ(redis.connection_string(), "redis://" + origin);
    EXPECT_EQ(redis.connection_string(3), "redis://" + origin + "/3");
    EXPECT_TRUE(redis.password().empty());
}

TEST_F(RedisModule, ExecSetGetRoundTrip) {
    const StartedRedis redis = RedisContainer().start();

    EXPECT_EQ(redis.container().exec({"redis-cli", "set", "greeting", "hello"}).exit_code, 0);

    const ExecResult get = redis.container().exec({"redis-cli", "get", "greeting"});
    EXPECT_EQ(get.exit_code, 0);
    EXPECT_NE(get.stdout_data.find("hello"), std::string::npos);
}

TEST_F(RedisModule, PasswordIsEnforcedAndWired) {
    const StartedRedis redis = RedisContainer().with_password("s3cr3t").start();

    // Auth is really on: an unauthenticated raw PING is refused...
    const std::string reply = tcit::redis_ping(redis.host(), redis.port());
    EXPECT_EQ(reply.substr(0, 7), "-NOAUTH");

    // ...while in-container redis-cli authenticates via REDISCLI_AUTH — the
    // same mechanism that made the startup wait pass.
    const ExecResult ping = redis.container().exec({"redis-cli", "ping"});
    EXPECT_EQ(ping.exit_code, 0);
    EXPECT_NE(ping.stdout_data.find("PONG"), std::string::npos);

    EXPECT_NE(redis.connection_string().find(":s3cr3t@"), std::string::npos);
    EXPECT_EQ(redis.password(), "s3cr3t");
}

TEST_F(RedisModule, CommandArgsReachTheServer) {
    const StartedRedis redis = RedisContainer().with_command_args({"--maxmemory", "64mb"}).start();

    const ExecResult res = redis.container().exec({"redis-cli", "config", "get", "maxmemory"});
    EXPECT_EQ(res.exit_code, 0);
    EXPECT_NE(res.stdout_data.find("67108864"), std::string::npos); // 64mb in bytes
}
