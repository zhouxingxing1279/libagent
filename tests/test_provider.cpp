#include "fakes/fake_provider.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace libagent;

// Validates the awaitable interface end-to-end with no network: a real
// io_context drives a FakeProvider coroutine.
TEST(FakeProvider, ChatReturnsScriptedResponse) {
    fakes::FakeProvider provider;
    ChatResponse scripted;
    scripted.message.role = Role::Assistant;
    scripted.message.content.text = "hello world";
    scripted.finish = FinishReason::Stop;
    provider.push(scripted);

    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<ChatResponse> {
            ChatRequest req;
            req.messages.push_back({Role::User, Content{"hi"}});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();

    const ChatResponse out = fut.get();
    EXPECT_EQ(out.message.content.text, "hello world");
    EXPECT_EQ(out.finish, FinishReason::Stop);
}

TEST(FakeProvider, StreamDeliversDeltaThenFinish) {
    fakes::FakeProvider provider;
    ChatResponse scripted;
    scripted.message.content.text = "chunk";
    scripted.finish = FinishReason::Length;
    scripted.usage = Usage{1, 2, 3};
    provider.push(scripted);

    std::vector<StreamEvent::Kind> kinds;
    std::string assembled;
    Usage final_usage{};

    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void> {
            ChatRequest req;
            TokenSink sink = [&](const StreamEvent& ev) -> boost::asio::awaitable<void> {
                kinds.push_back(ev.kind);
                if (ev.kind == StreamEvent::Kind::Delta) {
                    assembled += ev.delta;
                } else if (ev.kind == StreamEvent::Kind::Finish) {
                    final_usage = ev.usage;
                }
                co_return;
            };
            co_await provider.stream(req, sink);
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    ASSERT_EQ(kinds.size(), 2u);
    EXPECT_EQ(kinds[0], StreamEvent::Kind::Delta);
    EXPECT_EQ(kinds[1], StreamEvent::Kind::Finish);
    EXPECT_EQ(assembled, "chunk");
    EXPECT_EQ(final_usage.total_tokens, 3);
}

}  // namespace
