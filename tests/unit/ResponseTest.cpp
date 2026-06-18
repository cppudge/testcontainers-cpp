#include <gtest/gtest.h>

#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file:
//   Response.HeaderLookupIsCaseInsensitive - header() matches names case-insensitively and returns "" when absent.
//   Response.OkReflectsStatus - ok() is true only for 2xx status codes.

using namespace testcontainers;

TEST(Response, HeaderLookupIsCaseInsensitive) {
    Response r;
    r.headers = {{"Api-Version", "1.54"}, {"Content-Type", "text/plain"}};
    EXPECT_EQ(r.header("api-version"), "1.54");
    EXPECT_EQ(r.header("API-VERSION"), "1.54");
    EXPECT_EQ(r.header("Content-Type"), "text/plain");
    EXPECT_EQ(r.header("missing"), "");
}

TEST(Response, OkReflectsStatus) {
    Response r;
    r.status_code = 200;
    EXPECT_TRUE(r.ok());
    r.status_code = 204;
    EXPECT_TRUE(r.ok());
    r.status_code = 304;
    EXPECT_FALSE(r.ok());
    r.status_code = 404;
    EXPECT_FALSE(r.ok());
    r.status_code = 500;
    EXPECT_FALSE(r.ok());
}
