#include "libagent/tools/cli.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

using namespace libagent;
namespace cli = libagent::cli;

Json run_handler(const ToolHandler& handler, Json args) {
    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(
        ioc, handler(std::move(args)), boost::asio::use_future);
    ioc.run();
    return fut.get();
}

TEST(CliTool, CapturesStdoutStderrAndExit) {
    auto t = cli::command("echo_it", "echo via shell", {}, "/bin/sh",
                          {"-c", "echo hello; echo oops >&2; exit 3"});
    const Json r = run_handler(t.handler, Json::object());
    EXPECT_EQ(r["exit"], 3);
    EXPECT_EQ(r["stdout"], "hello\n");
    EXPECT_EQ(r["stderr"], "oops\n");
}

TEST(CliTool, SubstitutesArgsIntoArgv) {
    auto t = cli::command("upper", "print the arg",
                          {{"text", "string"}}, "/bin/sh",
                          {"-c", "echo {text}"});
    const Json r = run_handler(t.handler, Json{{"text", "world"}});
    EXPECT_EQ(r["stdout"], "world\n");
    EXPECT_EQ(r["exit"], 0);
}

TEST(CliTool, MissingProgramReturnsErrorJson) {
    auto t = cli::command("nope", "does not exist", {},
                          "/no/such/program/xyz", {});
    const Json r = run_handler(t.handler, Json::object());
    ASSERT_TRUE(r.contains("error"));
    EXPECT_FALSE(r["error"].get<std::string>().empty());
}

TEST(CliTool, SchemaDescribesObjectParams) {
    const auto t = cli::command("f", "d", {{"x", "number"}, {"y", "string"}},
                                "/bin/true", {});
    EXPECT_EQ(t.spec.name, "f");
    EXPECT_EQ(t.spec.parameters["type"], "object");
    EXPECT_EQ(t.spec.parameters["properties"]["x"]["type"], "number");
    EXPECT_EQ(t.spec.parameters["properties"]["y"]["type"], "string");
    ASSERT_TRUE(t.spec.parameters["required"].is_array());
    EXPECT_EQ(t.spec.parameters["required"].size(), 2u);
}

TEST(CliTool, CancellationPropagatesAndKillsChild) {
    // A long-running child; cancelling the handler must re-throw
    // operation_aborted (propagating to the agent) AND terminate the child so
    // io_context.run() doesn't hang on the 30s sleep.
    auto t = cli::command("sleeper", "sleeps", {}, "/bin/sh", {"-c", "sleep 30"});

    boost::asio::io_context ioc;
    boost::asio::cancellation_signal sig;
    auto fut = boost::asio::co_spawn(
        ioc, t.handler(Json::object()),
        boost::asio::bind_cancellation_slot(sig.slot(), boost::asio::use_future));

    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void> {
            boost::asio::steady_timer tm(co_await boost::asio::this_coro::executor,
                                         std::chrono::milliseconds(30));
            co_await tm.async_wait(boost::asio::use_awaitable);
            sig.emit(boost::asio::cancellation_type::all);
            co_return;
        },
        boost::asio::detached);
    ioc.run();

    EXPECT_THROW({ (void)fut.get(); }, std::exception);
}

}  // namespace
