#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ConnectionString.hpp"

// Tests in this file (the pure ConnectionString builder):
//   ConnectionString.FullUrlAssembles - scheme, credentials, host, port, database, and params compose in the canonical order.
//   ConnectionString.MinimalSchemeHostOnly - just scheme + host renders with no stray separators.
//   ConnectionString.OmittedComponentsEmitNoSeparators - no user -> no '@'; no port -> no ':'; no database -> no '/'; no params -> no '?'.
//   ConnectionString.PasswordWithoutUserKeepsUserinfo - a password-only userinfo renders as ":secret@" (empty user, RFC-valid).
//   ConnectionString.ComponentsArePercentEncoded - user/password/database/param keys and values are percent-encoded ('@' ':' '/' '&' '=' included); unreserved characters pass through.
//   ConnectionString.Ipv6HostBracketed - a bare IPv6 literal is bracketed; an already-bracketed one passes through.
//   ConnectionString.ParamsKeepInsertionOrder - query parameters render in with_param order.
//   ConnectionString.ChainsOnTemporary - the builder chains straight off the constructor expression.

using testcontainers::ConnectionString;

TEST(ConnectionString, FullUrlAssembles) {
    const std::string url = ConnectionString("postgresql")
                                .with_user("user")
                                .with_password("pass")
                                .with_host("localhost")
                                .with_port(5432)
                                .with_database("db")
                                .with_param("sslmode", "disable")
                                .to_string();
    EXPECT_EQ(url, "postgresql://user:pass@localhost:5432/db?sslmode=disable");
}

TEST(ConnectionString, MinimalSchemeHostOnly) {
    EXPECT_EQ(ConnectionString("redis").with_host("localhost").to_string(), "redis://localhost");
}

TEST(ConnectionString, OmittedComponentsEmitNoSeparators) {
    const std::string url =
        ConnectionString("mongodb").with_host("localhost").with_port(27017).to_string();
    EXPECT_EQ(url, "mongodb://localhost:27017");
    EXPECT_EQ(url.find('@'), std::string::npos);
    EXPECT_EQ(url.find('?'), std::string::npos);
    // No database: nothing follows the port.
    EXPECT_EQ(url.find('/', url.find("//") + 2), std::string::npos);
}

TEST(ConnectionString, PasswordWithoutUserKeepsUserinfo) {
    EXPECT_EQ(ConnectionString("amqp").with_password("secret").with_host("h").to_string(),
              "amqp://:secret@h");
}

TEST(ConnectionString, ComponentsArePercentEncoded) {
    const std::string url = ConnectionString("postgresql")
                                .with_user("us er")
                                .with_password("p@ss:w/rd")
                                .with_host("localhost")
                                .with_database("my db")
                                .with_param("a key", "v&l=ue")
                                .to_string();
    EXPECT_EQ(url, "postgresql://us%20er:p%40ss%3Aw%2Frd@localhost/my%20db?a%20key=v%26l%3Due");

    // Unreserved characters survive verbatim.
    EXPECT_EQ(ConnectionString("s").with_user("A-z.0_9~").with_host("h").to_string(),
              "s://A-z.0_9~@h");
}

TEST(ConnectionString, Ipv6HostBracketed) {
    EXPECT_EQ(ConnectionString("redis").with_host("::1").with_port(6379).to_string(),
              "redis://[::1]:6379");
    EXPECT_EQ(ConnectionString("redis").with_host("[fe80::1]").with_port(6379).to_string(),
              "redis://[fe80::1]:6379");
}

TEST(ConnectionString, ParamsKeepInsertionOrder) {
    EXPECT_EQ(ConnectionString("s")
                  .with_host("h")
                  .with_param("b", "2")
                  .with_param("a", "1")
                  .with_param("c", "3")
                  .to_string(),
              "s://h?b=2&a=1&c=3");
}

TEST(ConnectionString, ChainsOnTemporary) {
    // The plain lvalue-returning setters bind to a temporary too — the whole
    // build-and-render happens in one expression.
    const std::string url = ConnectionString("redis")
                                .with_host("localhost")
                                .with_port(6379)
                                .with_database("0")
                                .to_string();
    EXPECT_EQ(url, "redis://localhost:6379/0");
}
