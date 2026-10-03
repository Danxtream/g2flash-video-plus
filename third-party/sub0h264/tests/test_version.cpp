#include "doctest.h"
#include <sub0h264/sub0h264.hpp>

#include <cstring>

TEST_CASE("getVersionString returns non-empty string")
{
    const char* version = sub0h264::getVersionString();
    REQUIRE(version != nullptr);
    REQUIRE(std::strlen(version) > 0U);
    CHECK(std::strcmp(version, "0.1.0") == 0);
}

TEST_CASE("getVersion returns correct values")
{
    sub0h264::Version v = sub0h264::getVersion();
    CHECK(v.major_ == 0U);
    CHECK(v.minor_ == 1U);
    CHECK(v.patch_ == 0U);
}

TEST_CASE("platformName returns non-null")
{
    const char* name = sub0h264::platformName();
    REQUIRE(name != nullptr);
    REQUIRE(std::strlen(name) > 0U);
}
