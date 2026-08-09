#include "http/https_client.hpp"
#include "fakes/test_http_server.hpp"
#include "libagent/error.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/http.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <string_view>

namespace {

namespace http = libagent::http;
namespace ts = libagent::testsupport;
using boost::asio::awaitable;

TEST(HttpsClient, PlainRequestReturnsStatusAndBody) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"ok":true})";

    std::string got_target;
    std::string got_body;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_target, got_body),
                          boost::asio::detached);

    http::HttpsClient client;
    http::Request req;
    req.use_tls = false;
    req.host = "127.0.0.1";
    req.service = std::to_string(port);
    req.target = "/v1/test";
    req.method = boost::beast::http::verb::get;

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<http::Response> { co_return co_await client.request(req); },
        boost::asio::use_future);
    ioc.run();

    const http::Response out = fut.get();
    EXPECT_EQ(out.status, 200u);
    EXPECT_EQ(out.body, R"({"ok":true})");
    EXPECT_EQ(got_target, "/v1/test");
}

TEST(HttpsClient, StreamDeliversFullBodyViaChunks) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.content_type = "text/event-stream";
    resp.stream_chunks = {"data: {\"a\":1}\n\n", "data: {\"b\":2}\n\n",
                          "data: [DONE]\n\n"};

    std::string ignore_target;
    std::string ignore_body;
    boost::asio::co_spawn(ioc,
                          ts::serve_one(acceptor, resp, ignore_target, ignore_body),
                          boost::asio::detached);

    http::HttpsClient client;
    http::Request req;
    req.use_tls = false;
    req.host = "127.0.0.1";
    req.service = std::to_string(port);
    req.target = "/v1/chat/completions";
    req.method = boost::beast::http::verb::post;

    std::string assembled;
    unsigned got_status = 0;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<void> {
            auto on_chunk = [&](std::string_view chunk) -> awaitable<void> {
                assembled += std::string(chunk);
                co_return;
            };
            http::Response r = co_await client.request_stream(req, on_chunk);
            got_status = r.status;
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    EXPECT_EQ(got_status, 200u);
    EXPECT_EQ(assembled,
              "data: {\"a\":1}\n\ndata: {\"b\":2}\n\ndata: [DONE]\n\n");
}

TEST(HttpsClient, ReadTimeoutAbortsStalledServer) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.stall = true;  // accept + read, then never respond

    std::string ignore_target;
    std::string ignore_body;
    boost::asio::co_spawn(ioc,
                          ts::serve_one(acceptor, resp, ignore_target, ignore_body),
                          boost::asio::detached);

    http::HttpsClient client;
    http::Request req;
    req.use_tls = false;
    req.host = "127.0.0.1";
    req.service = std::to_string(port);
    req.target = "/slow";
    req.method = boost::beast::http::verb::get;
    req.timeout = std::chrono::milliseconds(150);

    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<http::Response> { co_return co_await client.request(req); },
        boost::asio::use_future);

    // The stalled server holds a pending op, so plain run() would block. Drive
    // one handler at a time until the client future is ready, then stop.
    while (ioc.run_one()) {
        if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            break;
        }
    }
    ioc.stop();

    try {
        (void)fut.get();
        FAIL() << "expected a timeout Error";
    } catch (const libagent::Error& e) {
        EXPECT_EQ(e.code(), libagent::ErrorCode::Timeout);
    }
}

}  // namespace
