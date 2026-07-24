#include "https_client.hpp"

#include "libagent/version.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <stdexcept>

#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace libagent::http {

namespace {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using net::ip::tcp;
using ssl_stream = net::ssl::stream<tcp::socket>;

bool has_content_type(const Request& req) {
    for (const auto& [k, v] : req.headers) {
        if (k.size() == 12 && beast::iequals(k, "content-type")) {
            (void)v;
            return true;
        }
    }
    return false;
}

void apply_common_headers(http::request<http::string_body>& hr, const Request& req) {
    hr.set(http::field::host, req.host);
    hr.set(http::field::user_agent, "libagent/" LIBAGENT_VERSION);
    if (!req.body.empty() && !has_content_type(req)) {
        hr.set(http::field::content_type, "application/json");
    }
    for (const auto& [k, v] : req.headers) {
        hr.set(k, v);
    }
}

http::request<http::string_body> make_request(const Request& req) {
    http::request<http::string_body> hr{req.method, req.target, 11, req.body};
    apply_common_headers(hr, req);
    hr.prepare_payload();
    return hr;
}

// Best-effort connection shutdown. SSL gets an async TLS close_notify (stream
// truncation errors are ignored); plain sockets close immediately.
template <class Stream>
net::awaitable<void> close_stream(Stream& stream) {
    boost::system::error_code ec;
    if constexpr (std::is_same_v<Stream, ssl_stream>) {
        co_await stream.async_shutdown(net::redirect_error(net::use_awaitable, ec));
    } else {
        stream.shutdown(tcp::socket::shutdown_both, ec);
    }
    (void)ec;
}

template <class Stream>
net::awaitable<Response> round_trip(Stream& stream, const Request& req) {
    http::request<http::string_body> hr = make_request(req);
    co_await http::async_write(stream, hr, net::use_awaitable);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buf, res, net::use_awaitable);

    Response out;
    out.status = res.result_int();
    out.body = std::move(res.body());
    for (auto& f : res) {
        out.headers.emplace_back(std::string(f.name_string()), std::string(f.value()));
    }

    co_await close_stream(stream);
    co_return out;
}

template <class Stream>
net::awaitable<Response> stream_body(
    Stream& stream, const Request& req,
    const std::function<net::awaitable<void>(std::string_view)>& on_chunk) {
    http::request<http::string_body> hr = make_request(req);
    co_await http::async_write(stream, hr, net::use_awaitable);

    beast::flat_buffer buf;
    http::response_parser<http::buffer_body> parser;
    co_await http::async_read_header(stream, buf, parser, net::use_awaitable);

    Response out;
    out.status = parser.get().result_int();
    for (auto& f : parser.get()) {
        out.headers.emplace_back(std::string(f.name_string()), std::string(f.value()));
    }

    char work[8192];
    boost::system::error_code ec;
    while (!parser.is_done()) {
        parser.get().body().data = work;
        parser.get().body().size = sizeof(work);
        co_await http::async_read_some(stream, buf, parser,
                                       net::redirect_error(net::use_awaitable, ec));
        const std::size_t avail = sizeof(work) - parser.get().body().size;
        if (avail > 0) {
            co_await on_chunk(std::string_view(work, avail));
        }
        if (ec == http::error::need_buffer) {
            ec = {};
            continue;
        }
        if (ec) {
            ec = {};
            break;  // connection closed / end of stream
        }
    }

    co_await close_stream(stream);
    co_return out;
}

}  // namespace

net::awaitable<Response> HttpsClient::request(const Request& req) {
    auto ex = co_await net::this_coro::executor;
    tcp::resolver resolver(ex);
    const auto results =
        co_await resolver.async_resolve(req.host, req.service, net::use_awaitable);

    if (req.use_tls) {
        net::ssl::context ctx(net::ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        ssl_stream stream(ex, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), req.host.c_str())) {
            throw std::runtime_error("libagent: SNI setup failed for " + req.host);
        }
        co_await net::async_connect(stream.lowest_layer(), results, net::use_awaitable);
        co_await stream.async_handshake(net::ssl::stream_base::client, net::use_awaitable);
        co_return co_await round_trip(stream, req);
    }
    tcp::socket stream(ex);
    co_await net::async_connect(stream, results, net::use_awaitable);
    co_return co_await round_trip(stream, req);
}

net::awaitable<Response> HttpsClient::request_stream(
    const Request& req,
    std::function<net::awaitable<void>(std::string_view)> on_chunk) {
    auto ex = co_await net::this_coro::executor;
    tcp::resolver resolver(ex);
    const auto results =
        co_await resolver.async_resolve(req.host, req.service, net::use_awaitable);

    if (req.use_tls) {
        net::ssl::context ctx(net::ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        ssl_stream stream(ex, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), req.host.c_str())) {
            throw std::runtime_error("libagent: SNI setup failed for " + req.host);
        }
        co_await net::async_connect(stream.lowest_layer(), results, net::use_awaitable);
        co_await stream.async_handshake(net::ssl::stream_base::client, net::use_awaitable);
        co_return co_await stream_body(stream, req, on_chunk);
    }
    tcp::socket stream(ex);
    co_await net::async_connect(stream, results, net::use_awaitable);
    co_return co_await stream_body(stream, req, on_chunk);
}

}  // namespace libagent::http
