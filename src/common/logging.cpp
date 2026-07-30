#include "common/log.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <mutex>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace mosaic::common::log {
namespace {

// Sinks shared by every category logger, plus the configured level. Guarded by g_mu because
// category() may run on worker threads while the UI thread reconfigures via init().
std::mutex g_mu;
std::vector<spdlog::sink_ptr> g_sinks;
spdlog::level::level_enum g_level = spdlog::level::info;

// [12:34:56.789] [warn    ] [render] message
//   ^time         ^level     ^category(%n)
// %^...%$ colorizes the level on color sinks; other sinks ignore the markers.
constexpr const char* kPattern = "[%H:%M:%S.%e] [%^%-8l%$] [%n] %v";

spdlog::level::level_enum toSpd(Level level) {
    switch (level) {
        case Level::Trace: return spdlog::level::trace;
        case Level::Debug: return spdlog::level::debug;
        case Level::Info: return spdlog::level::info;
        case Level::Warn: return spdlog::level::warn;
        case Level::Error: return spdlog::level::err;
        case Level::Critical: return spdlog::level::critical;
        case Level::Off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

// Lazily install a colorized stderr sink so logging works before init() is called.
void ensureSinksLocked() {
    if (!g_sinks.empty()) return;
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    sink->set_pattern(kPattern);
    g_sinks.push_back(std::move(sink));
}

}  // namespace

void init(const Options& opts, std::string* error) {
    std::lock_guard<std::mutex> lk(g_mu);

    std::vector<spdlog::sink_ptr> sinks;

    spdlog::sink_ptr console;
    if (opts.color) {
        console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    } else {
        console = std::make_shared<spdlog::sinks::stderr_sink_mt>();
    }
    console->set_pattern(kPattern);
    sinks.push_back(std::move(console));

    if (!opts.file.empty()) {
        try {
            // Append (don't truncate): keep history across runs until rotation lands later.
            auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(opts.file, false);
            file->set_pattern(kPattern);
            sinks.push_back(std::move(file));
        } catch (const std::exception& e) {
            if (error) {
                *error = "could not open log file '" + opts.file + "': " + e.what();
            }
            // Leave stderr logging intact rather than failing the whole program.
        }
    }

    g_sinks = std::move(sinks);
    g_level = toSpd(opts.level);

    // Re-point any loggers already created (e.g. created before init(), or on a later
    // reconfigure) at the new sinks/level. g_sinks/g_level are namespace globals read under the
    // g_mu we already hold; the lambda only touches the logger object, never the spdlog
    // registry, so there is no lock-order inversion with apply_all's registry lock.
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
        logger->sinks() = g_sinks;
        logger->set_level(g_level);
        logger->flush_on(spdlog::level::warn);
    });
}

[[nodiscard]] std::optional<Level> parseLevel(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "trace") return Level::Trace;
    if (lower == "debug") return Level::Debug;
    if (lower == "info") return Level::Info;
    if (lower == "warn" || lower == "warning") return Level::Warn;
    if (lower == "error" || lower == "err") return Level::Error;
    if (lower == "critical" || lower == "crit") return Level::Critical;
    if (lower == "off" || lower == "none") return Level::Off;
    return std::nullopt;
}

[[nodiscard]] std::string_view levelName(Level level) {
    switch (level) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
        case Level::Critical: return "critical";
        case Level::Off: return "off";
    }
    return "info";
}

[[nodiscard]] std::shared_ptr<spdlog::logger> category(std::string_view name) {
    std::string key(name);
    std::lock_guard<std::mutex> lk(g_mu);
    if (auto existing = spdlog::get(key)) return existing;

    ensureSinksLocked();
    auto logger = std::make_shared<spdlog::logger>(key, g_sinks.begin(), g_sinks.end());
    logger->set_level(g_level);
    logger->flush_on(spdlog::level::warn);
    spdlog::register_logger(logger);
    return logger;
}

}  // namespace mosaic::common::log
