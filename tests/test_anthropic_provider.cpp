#include "libagent/providers/anthropic.hpp"
#include "libagent/error.hpp"
#include "fakes/test_http_server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace libagent;
namespace ts = libagent::testsupport;
using boost::asio::awaitable;

ChatResponse run_chat(anthropic::AnthropicProvider& provider, boost::asio::io_context& ioc,
                      ChatRequest req) {
    auto fut = boost::asio::co_spawn(
        ioc, [&]() -> awaitable<ChatResponse> { co_return co_await provider.chat(req); },
        boost::asio::use_future);
    ioc.run();
    return fut.get();
}

std::string ok_body() {
    return R"({"content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn"})";
}

TEST(AnthropicProvider, ChatParsesTextAndUsage) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"content":[{"type":"text","text":"Hello!"}],)"
               R"("stop_reason":"end_turn","usage":{"input_tokens":10,"output_tokens":2}})";

    std::string ig_t, ig_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, ig_t, ig_b), boost::asio::detached);

    anthropic::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    anthropic::AnthropicProvider provider(std::move(opts));

    ChatRequest req;
    req.messages.push_back({Role::User, Content{"hi"}});
    const ChatResponse out = run_chat(provider, ioc, req);
    EXPECT_EQ(out.message.content.text, "Hello!");
    EXPECT_EQ(out.finish, FinishReason::Stop);
    EXPECT_EQ(out.usage.prompt_tokens, 10);
    EXPECT_EQ(out.usage.completion_tokens, 2);
}

TEST(AnthropicProvider, ChatParsesToolUse) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"content":[{"type":"tool_use","id":"toolu_1",)"
               R"("name":"get_weather","input":{"city":"SF"}}],"stop_reason":"tool_use"})";

    std::string ig_t, ig_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, ig_t, ig_b), boost::asio::detached);

    anthropic::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    anthropic::AnthropicProvider provider(std::move(opts));

    ChatRequest req;
    req.messages.push_back({Role::User, Content{"weather?"}});
    const ChatResponse out = run_chat(provider, ioc, req);
    EXPECT_EQ(out.finish, FinishReason::ToolCalls);
    ASSERT_EQ(out.message.tool_calls.size(), 1u);
    EXPECT_EQ(out.message.tool_calls[0].name, "get_weather");
    EXPECT_EQ(out.message.tool_calls[0].arguments["city"], "SF");
}

TEST(AnthropicProvider, SystemPromptIsTopLevel) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = ok_body();

    std::string got_t, got_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_t, got_b), boost::asio::detached);

    anthropic::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    anthropic::AnthropicProvider provider(std::move(opts));

    ChatRequest req;
    req.messages.push_back({Role::System, Content{"You are helpful."}});
    req.messages.push_back({Role::User, Content{"hi"}});
    run_chat(provider, ioc, req);

    const Json body = Json::parse(got_b, nullptr, false);
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body["system"], "You are helpful.");
    ASSERT_TRUE(body["messages"].is_array());
    EXPECT_EQ(body["messages"][0]["role"], "user");  // system NOT in messages
}

TEST(AnthropicProvider, ConsecutiveToolResultsMergeIntoOneUserTurn) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = ok_body();

    std::string got_t, got_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_t, got_b), boost::asio::detached);

    anthropic::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    anthropic::AnthropicProvider provider(std::move(opts));

    ChatRequest req;
    req.messages.push_back({Role::User, Content{"q"}});
    Message asst{Role::Assistant};
    asst.tool_calls.push_back({"t1", "f", Json::object()});
    asst.tool_calls.push_back({"t2", "g", Json::object()});
    req.messages.push_back(asst);
    Message tr1{Role::Tool};
    tr1.tool_call_id = "t1";
    tr1.content.text = "r1";
    Message tr2{Role::Tool};
    tr2.tool_call_id = "t2";
    tr2.content.text = "r2";
    req.messages.push_back(tr1);
    req.messages.push_back(tr2);
    run_chat(provider, ioc, req);

    const Json body = Json::parse(got_b, nullptr, false);
    const Json& msgs = body["messages"];
    // [user("q"), assistant[tool_use x2], user[tool_result x2]]
    ASSERT_EQ(msgs.size(), 3u);
    ASSERT_EQ(msgs[2]["role"], "user");
    ASSERT_EQ(msgs[2]["content"].size(), 2u);
    EXPECT_EQ(msgs[2]["content"][0]["type"], "tool_result");
    EXPECT_EQ(msgs[2]["content"][0]["tool_use_id"], "t1");
    EXPECT_EQ(msgs[2]["content"][1]["tool_use_id"], "t2");
}

TEST(AnthropicProvider, MultimodalUserSerializesBase64Image) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = ok_body();

    std::string got_t, got_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_t, got_b), boost::asio::detached);

    anthropic::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    anthropic::AnthropicProvider provider(std::move(opts));

    ChatRequest req;
    Message m;
    m.role = Role::User;
    m.content.text = "describe this";
    ImageRef img;
    img.url = "ignored";
    img.media_type = "image/png";
    img.data = "BASE64DATA";
    m.content.images.push_back(img);
    req.messages.push_back(m);
    run_chat(provider, ioc, req);

    const Json body = Json::parse(got_b, nullptr, false);
    const Json& content = body["messages"][0]["content"];
    ASSERT_TRUE(content.is_array());
    ASSERT_EQ(content.size(), 2u);
    EXPECT_EQ(content[0]["type"], "text");
    EXPECT_EQ(content[1]["type"], "image");
    EXPECT_EQ(content[1]["source"]["type"], "base64");
    EXPECT_EQ(content[1]["source"]["media_type"], "image/png");
    EXPECT_EQ(content[1]["source"]["data"], "BASE64DATA");
}

}  // namespace
