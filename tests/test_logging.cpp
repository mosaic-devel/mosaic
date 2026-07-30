#include "common/log.hpp"

#include <doctest/doctest.h>

#include <initializer_list>

using namespace mosaic::common;

TEST_CASE("log levels parse case-insensitively with aliases") {
    CHECK(log::parseLevel("trace") == log::Level::Trace);
    CHECK(log::parseLevel("INFO") == log::Level::Info);
    CHECK(log::parseLevel("Warn") == log::Level::Warn);
    CHECK(log::parseLevel("warning") == log::Level::Warn);
    CHECK(log::parseLevel("off") == log::Level::Off);
    CHECK_FALSE(log::parseLevel("nonsense").has_value());
}

TEST_CASE("levelName is the inverse of parseLevel") {
    for (const log::Level level :
         {log::Level::Trace, log::Level::Debug, log::Level::Info, log::Level::Warn,
          log::Level::Error, log::Level::Critical, log::Level::Off}) {
        CHECK(log::parseLevel(log::levelName(level)) == level);
    }
}

TEST_CASE("category returns one stable logger per name") {
    const auto a = log::category("test-category");
    const auto b = log::category("test-category");
    REQUIRE(a != nullptr);
    CHECK(a == b);  // same shared instance for the same name
    CHECK(a->name() == "test-category");

    const auto other = log::category("other-category");
    CHECK(other != a);
}

TEST_CASE("init reconfigures the level of existing loggers") {
    const auto cat = log::category("test-reconfig");
    log::init({.level = log::Level::Error, .file = {}, .color = false});
    CHECK(cat->level() == spdlog::level::err);
    log::init({.level = log::Level::Debug, .file = {}, .color = false});
    CHECK(cat->level() == spdlog::level::debug);
}
