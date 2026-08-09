#include "libagent/tools/http.hpp"
#include "libagent/json.hpp"
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

TEST(HttpGetTool, FetchesUrl) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = "hello-body";

    std::string got_target, got_body;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, got_target, got_body),
                          boost::asio::detached);

    auto t = tools::http_get("fetch", "fetch a URL");
    Json result;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<Json> {
            co_return co_await t.handler(
                Json{{"url", "http://127.0.0.1:" + std::to_string(port) + "/path"}});
        },
        boost::asio::use_future);
    ioc.run();
    result = fut.get();

    EXPECT_EQ(result["status"], 200);
    EXPECT_EQ(result["body"], "hello-body");
    EXPECT_EQ(got_target, "/path");
}

}  // namespace
