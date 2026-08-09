#include "libagent/providers/openai.hpp"
#include "libagent/error.hpp"
#include "fakes/test_http_server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

using namespace libagent;
namespace ts = libagent::testsupport;
using boost::asio::awaitable;

TEST(OpenAiProvider, ChatParsesMessageUsageAndRequestShape) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"choices":[{"message":{"role":"assistant","content":"Hello!"},)"
               R"("finish_reason":"stop"}],"usage":{"prompt_tokens":5,)"
               R"("completion_tokens":2,"total_tokens":7}})";

    std::string got_target;
    std::string got_body;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_target, got_body),
                          boost::asio::detached);

    openai::Options opts;
    opts.api_key = "test-key";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            req.options.model = "gpt-test";
            req.messages.push_back({Role::User, Content{"hi"}});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();

    const ChatResponse out = fut.get();
    EXPECT_EQ(out.message.content.text, "Hello!");
    EXPECT_EQ(out.finish, FinishReason::Stop);
    EXPECT_EQ(out.usage.total_tokens, 7);

    // The provider should have POSTed to the chat path with the requested model.
    EXPECT_EQ(got_target, "/v1/chat/completions");
    const Json body = Json::parse(got_body, nullptr, false);
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body["model"], "gpt-test");
    ASSERT_TRUE(body["messages"].is_array());
    EXPECT_EQ(body["messages"][0]["role"], "user");
}

TEST(OpenAiProvider, ChatParsesToolCalls) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"choices":[{"message":{"role":"assistant","content":null,)"
               R"("tool_calls":[{"id":"c1","type":"function","function":)"
               R"({"name":"get_weather","arguments":"{\"city\":\"SF\"}"}}]},)"
               R"("finish_reason":"tool_calls"}]})";

    std::string ignore_t;
    std::string ignore_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, ignore_t, ignore_b),
                          boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            req.messages.push_back({Role::User, Content{"weather?"}});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();

    const ChatResponse out = fut.get();
    EXPECT_EQ(out.finish, FinishReason::ToolCalls);
    ASSERT_EQ(out.message.tool_calls.size(), 1u);
    EXPECT_EQ(out.message.tool_calls[0].name, "get_weather");
    EXPECT_EQ(out.message.tool_calls[0].arguments["city"], "SF");
}

TEST(OpenAiProvider, ToolSchemaCoercedToObjectType) {
    // OpenAI/DeepSeek reject function schemas without type:"object". The
    // provider must coerce a bare {} parameters schema.
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"choices":[{"message":{"role":"assistant","content":"ok"},)"
               R"("finish_reason":"stop"}]})";

    std::string got_target;
    std::string got_body;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_target, got_body),
                          boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            req.options.tools.push_back({"noargs", "needs no args", Json::object()});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    const Json body = Json::parse(got_body, nullptr, false);
    ASSERT_TRUE(body.is_object());
    ASSERT_TRUE(body["tools"].is_array());
    ASSERT_EQ(body["tools"].size(), 1u);
    EXPECT_EQ(body["tools"][0]["type"], "function");
    EXPECT_EQ(body["tools"][0]["function"]["name"], "noargs");
    EXPECT_EQ(body["tools"][0]["function"]["parameters"]["type"], "object");
}

TEST(OpenAiProvider, StreamAssemblesDeltasAndFinish) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.content_type = "text/event-stream";
    resp.stream_chunks = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n",
        "data: [DONE]\n\n",
    };

    std::string ignore_t;
    std::string ignore_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, ignore_t, ignore_b),
                          boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    openai::OpenAiProvider provider(std::move(opts));

    std::string assembled;
    FinishReason finish = FinishReason::Error;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<void> {
            TokenSink sink = [&](const StreamEvent& ev) -> awaitable<void> {
                if (ev.kind == StreamEvent::Kind::Delta) {
                    assembled += ev.delta;
                } else if (ev.kind == StreamEvent::Kind::Finish) {
                    finish = ev.finish;
                }
                co_return;
            };
            ChatRequest req;
            req.messages.push_back({Role::User, Content{"hi"}});
            co_await provider.stream(req, sink);
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    EXPECT_EQ(assembled, "Hello");
    EXPECT_EQ(finish, FinishReason::Stop);
}

TEST(OpenAiProvider, HttpErrorIsTyped) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 401;  // auth error — not retried, surfaces as a typed Error
    resp.body = R"({"error":"invalid api key"})";

    std::string a, b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, a, b), boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    opts.max_retries = 0;
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            req.messages.push_back({Role::User, Content{"hi"}});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();

    try {
        (void)fut.get();
        FAIL() << "expected an auth Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Auth);
        EXPECT_EQ(e.http_status(), 401);
    }
}

TEST(OpenAiProvider, RetriesOn429ThenSucceeds) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse r429;
    r429.status = 429;
    r429.body = R"({"error":"rate limit"})";
    ts::CannedResponse r200;
    r200.status = 200;
    r200.body = R"({"choices":[{"message":{"role":"assistant","content":"ok"},)"
               R"("finish_reason":"stop"}]})";

    std::string a, b, c, d;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, r429, a, b), boost::asio::detached);
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, r200, c, d), boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    opts.max_retries = 2;
    opts.initial_backoff = std::chrono::milliseconds(5);
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            req.messages.push_back({Role::User, Content{"hi"}});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();

    const ChatResponse out = fut.get();
    EXPECT_EQ(out.message.content.text, "ok");
}

TEST(OpenAiProvider, NonRetryableStatusThrowsImmediately) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse r400;
    r400.status = 400;  // client error — not retried
    r400.body = R"({"error":"bad request"})";

    std::string a, b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, r400, a, b), boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    opts.max_retries = 3;
    opts.initial_backoff = std::chrono::milliseconds(5);
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            req.messages.push_back({Role::User, Content{"hi"}});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();

    EXPECT_THROW({ (void)fut.get(); }, std::runtime_error);
}

TEST(OpenAiProvider, ExhaustsRetriesThenThrows) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse r503a;
    r503a.status = 503;
    r503a.body = R"({"error":"unavailable"})";
    ts::CannedResponse r503b = r503a;

    std::string a, b, c, d;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, r503a, a, b), boost::asio::detached);
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, r503b, c, d), boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    opts.max_retries = 1;  // 1 retry => 2 attempts total
    opts.initial_backoff = std::chrono::milliseconds(5);
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            req.messages.push_back({Role::User, Content{"hi"}});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();

    EXPECT_THROW({ (void)fut.get(); }, std::runtime_error);
}

TEST(OpenAiProvider, StructuredOutputFieldsAreSerialized) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"choices":[{"message":{"role":"assistant","content":"{}"},)"
               R"("finish_reason":"stop"}]})";

    std::string got_t, got_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_t, got_b), boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    opts.max_retries = 0;
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            req.options.response_format = Json{{"type", "json_object"}};
            req.options.tool_choice = "required";
            req.options.seed = 42L;
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    const Json body = Json::parse(got_b, nullptr, false);
    EXPECT_EQ(body["response_format"]["type"], "json_object");
    EXPECT_EQ(body["tool_choice"], "required");
    EXPECT_EQ(body["seed"], 42);
}

TEST(OpenAiProvider, MultimodalUserSerializesImageUrl) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"choices":[{"message":{"role":"assistant","content":"ok"},)"
               R"("finish_reason":"stop"}]})";

    std::string got_t, got_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_t, got_b), boost::asio::detached);

    openai::Options opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    opts.max_retries = 0;
    openai::OpenAiProvider provider(std::move(opts));

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<ChatResponse> {
            ChatRequest req;
            Message m;
            m.role = Role::User;
            m.content.text = "what's this?";
            ImageRef img;
            img.url = "https://example.com/x.png";
            m.content.images.push_back(img);
            req.messages.push_back(m);
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    const Json body = Json::parse(got_b, nullptr, false);
    const Json& content = body["messages"][0]["content"];
    ASSERT_TRUE(content.is_array());
    ASSERT_EQ(content.size(), 2u);
    EXPECT_EQ(content[0]["type"], "text");
    EXPECT_EQ(content[1]["type"], "image_url");
    EXPECT_EQ(content[1]["image_url"]["url"], "https://example.com/x.png");
}

}  // namespace
