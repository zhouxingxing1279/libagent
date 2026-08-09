#include "libagent/logging/spdlog.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <gtest/gtest.h>

#include <sstream>
#include <string>

TEST(SpdlogAdapter, ForwardsLevelAndMessage) {
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(oss);
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("%v");  // just the message
    spdlog::set_default_logger(logger);

    libagent::LogSink libagent_sink = libagent::make_spdlog_sink();
    libagent_sink(libagent::LogLevel::Warn, "hello-warn");

    spdlog::shutdown();
    EXPECT_NE(oss.str().find("hello-warn"), std::string::npos);
}

TEST(SpdlogAdapter, TargetsALogger) {
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(oss);
    auto logger = std::make_shared<spdlog::logger>("named", sink);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("%v");

    libagent::LogSink libagent_sink = libagent::make_spdlog_sink(logger);
    libagent_sink(libagent::LogLevel::Info, "to-named");
    logger->flush();

    EXPECT_NE(oss.str().find("to-named"), std::string::npos);
}
