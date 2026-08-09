#include "libagent/providers/openai_embeddings.hpp"
#include "libagent/json.hpp"
#include "fakes/test_http_server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace libagent;
namespace ts = libagent::testsupport;
using boost::asio::awaitable;

TEST(OpenAiEmbedder, ParsesEmbeddingVector) {
    boost::asio::io_context ioc;
    auto acceptor = ts::make_local_acceptor(ioc);
    const unsigned port = acceptor.local_endpoint().port();

    ts::CannedResponse resp;
    resp.status = 200;
    resp.body = R"({"data":[{"embedding":[0.1,0.2,0.3,0.4]}]})";

    std::string ig_t, ig_b;
    boost::asio::co_spawn(ioc, ts::serve_one(acceptor, resp, ig_t, ig_b), boost::asio::detached);

    openai::EmbedderOptions opts;
    opts.api_key = "k";
    opts.base_url = "http://127.0.0.1:" + std::to_string(port);
    openai::Embedder embedder(std::move(opts));

    std::vector<float> vec;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<std::vector<float>> { co_return co_await embedder.embed("hello"); },
        boost::asio::use_future);
    ioc.run();
    vec = fut.get();

    ASSERT_EQ(vec.size(), 4u);
    EXPECT_FLOAT_EQ(vec[0], 0.1F);
    EXPECT_FLOAT_EQ(vec[3], 0.4F);

    // The request carried the text as "input".
    const Json body = Json::parse(ig_b, nullptr, false);
    EXPECT_EQ(body["input"], "hello");
}

}  // namespace
