#pragma once

// Optional adapter: route libagent's LogSink to spdlog. Include this header and
// link spdlog (enable with CMake -DLIBAGENT_WITH_SPDLOG=ON). The core libagent
// library does NOT depend on spdlog.

#include "libagent/logging.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace libagent {

inline spdlog::level::level_enum to_spdlog_level(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info: return spdlog::level::info;
        case LogLevel::Warn: return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
    }
    return spdlog::level::info;
}

/// A LogSink that forwards to spdlog's default logger.
inline LogSink make_spdlog_sink() {
    return [](LogLevel level, const std::string& msg) {
        spdlog::log(to_spdlog_level(level), msg);
    };
}

/// A LogSink that forwards to a specific spdlog logger.
inline LogSink make_spdlog_sink(std::shared_ptr<spdlog::logger> logger) {
    return [logger = std::move(logger)](LogLevel level, const std::string& msg) {
        logger->log(to_spdlog_level(level), msg);
    };
}

}  // namespace libagent
