#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "common/version.hpp"
#include "core/core.hpp"
#include "io/io.hpp"
#include "platform/platform.hpp"
#include "render/render.hpp"
#include "ui/ui.hpp"

#include <string>
#include <string_view>

TEST_CASE("version metadata is populated") {
    CHECK(mosaic::common::appName() == "Mosaic");
    // The version is FROZEN at 0.2.17 ("perma-alpha", PLAN.md item 9) until 1.0.0; assert the
    // shape rather than the literal so the one legitimate future change (1.0.0) is a one-line edit.
    const std::string_view version = mosaic::common::appVersion();
    CHECK_FALSE(version.empty());
    CHECK(version.substr(0, 2) == "0.");
    // buildInfo() embeds the app name and the version (+ the git commit + build type).
    const std::string info = mosaic::common::buildInfo();
    CHECK(info.find("Mosaic") != std::string::npos);
    CHECK(info.find(version) != std::string::npos);
}

TEST_CASE("module identities are wired up") {
    CHECK(mosaic::core::moduleName() == "core");
    CHECK(mosaic::render::moduleName() == "render");
    CHECK(mosaic::io::moduleName() == "io");
    CHECK(mosaic::ui::moduleName() == "ui");
    CHECK(mosaic::platform::moduleName() == "platform");
}
