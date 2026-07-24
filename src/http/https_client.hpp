#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/verb.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libagent::http {

struct Request {
    std::string host;
    std::string service = "443";      // port
    std::string target = "/";         // path?query
    boost::beast::http::verb method = boost::beast::http::verb::post;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool use_tls = true;
};

struct Response {
    unsigned status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

/// Coroutine HTTPS/HTTP client built on Boost.Asio + Beast + OpenSSL.
/// Providers call this so their code stays about serialization, not transport.
/// Stateless: each request coroutine resolves its executor from the caller's
/// coroutine context via this_coro::executor.
class HttpsClient {
public:
    HttpsClient() = default;

    /// One-shot request/response.
    boost::asio::awaitable<Response> request(const Request& req);

    /// Streaming response: after the status line + headers are read, each body
    /// chunk is delivered to `on_chunk` (awaitable sink = real backpressure).
    /// The final Response carries status + headers (body is empty).
    boost::asio::awaitable<Response> request_stream(
        const Request& req,
        std::function<boost::asio::awaitable<void>(std::string_view chunk)> on_chunk);
};

}  // namespace libagent::http
