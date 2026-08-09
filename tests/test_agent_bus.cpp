#include "libagent/agent_bus.hpp"
#include "libagent/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace libagent;
using boost::asio::awaitable;

TEST(AgentBus, PostsAndReceivesMessage) {
    boost::asio::io_context ioc;
    AgentBus bus(ioc.get_executor());
    bus.register_agent("alice");
    bus.register_agent("bob");

    Message received;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<void> {
            co_await bus.post("alice", {Role::User, Content{"hi from bob"}});
            received = co_await bus.await_message("alice");
            co_return;
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    EXPECT_EQ(received.role, Role::User);
    EXPECT_EQ(received.content.text, "hi from bob");
}

TEST(AgentBus, BufferedMessagesAreAllDelivered) {
    boost::asio::io_context ioc;
    AgentBus bus(ioc.get_executor());
    bus.register_agent("x");

    std::vector<std::string> got;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<void> {
            co_await bus.post("x", {Role::User, Content{"one"}});
            co_await bus.post("x", {Role::User, Content{"two"}});
            got.push_back((co_await bus.await_message("x")).content.text);
            got.push_back((co_await bus.await_message("x")).content.text);
            co_return;
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "one");
    EXPECT_EQ(got[1], "two");
}

TEST(AgentBus, AwaitErrorsOnClose) {
    boost::asio::io_context ioc;
    AgentBus bus(ioc.get_executor());
    bus.register_agent("y");

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<Message> { co_return co_await bus.await_message("y"); },
        boost::asio::use_future);
    bus.close("y");  // close before run -> await completes with error
    ioc.run();

    EXPECT_THROW({ (void)fut.get(); }, boost::system::system_error);
}

}  // namespace
