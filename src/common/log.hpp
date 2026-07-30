#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

// Mosaic's logging facade, a thin layer over spdlog (MIT). It exists so the rest of the
// codebase logs through one place with consistent **categories** (one named logger per
// module: "app", "ui", "render", "io", "core", "platform") sharing a single set of sinks
// (colorized stderr + an optional log file), one configurable **level**, and one pattern.
//
// Usage:
//     mosaic::common::log::category("render")->warn("device lost: {}", reason);
// Categories are created on first use, so no registration step is needed. Logging works at a
// default level before init() runs (so nothing is lost during early start-up); main() calls
// init() once it has parsed the CLI / loaded settings.
//
// Note: hot paths (pixel kernels, the render submit loop) use std::expected / error codes and
// must NOT log per-pixel; logging is for diagnostics, not the inner loop.
namespace mosaic::common::log {

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

struct Options {
    Level level = Level::Info;
    std::string file;   // empty => stderr only; otherwise also append to this path
    bool color = true;  // colorize the stderr sink
};

// Configure the sinks and level. Safe to call once at start-up; calling again reconfigures
// every existing category logger (handy for a live "log level" change from settings). Never
// throws: a sink that cannot be created (e.g. an unwritable file) is reported via `error`
// (if non-null) and skipped, leaving stderr logging intact.
void init(const Options& opts, std::string* error = nullptr);

// Parse "trace|debug|info|warn|warning|error|critical|off" (case-insensitive).
[[nodiscard]] std::optional<Level> parseLevel(std::string_view name);

// Canonical lowercase name for a level ("trace".."off"); the inverse of parseLevel().
[[nodiscard]] std::string_view levelName(Level level);

// The logger for `name`, created (sharing the current sinks/level) on first use. Thread-safe.
[[nodiscard]] std::shared_ptr<spdlog::logger> category(std::string_view name);

}  // namespace mosaic::common::log
