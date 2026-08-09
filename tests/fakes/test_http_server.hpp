#pragma once

// Single-shot HTTP/1.1 test server (plain TCP, no TLS) for client/provider
// tests. Accepts one connection, reads the request, writes a canned response.

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace libagent::testsupport {

struct CannedResponse {
    unsigned status = 200;
    std::string content_type = "application/json";
    std::string body;
    // If non-empty, the body is sent as one write per entry (for SSE/streaming
    // tests). Content-Length is the sum of all entries; `body` is ignored.
    std::vector<std::string> stream_chunks;
    // If true, accept + read the request, then never respond (keep the socket
    // open). Used to exercise client read timeouts.
    bool stall = false;
};

// Acceptor bound to an ephemeral port on the loopback interface.
inline boost::asio::ip::tcp::acceptor make_local_acceptor(boost::asio::io_context& ioc) {
    using boost::asio::ip::tcp;
    return tcp::acceptor(ioc, tcp::endpoint(tcp::v4(), 0));
}

// Accept one connection, read the request, write `resp`, then close.
// The received target and body are stored via out-parameters.
inline boost::asio::awaitable<void> serve_one(
    boost::asio::ip::tcp::acceptor& acceptor, const CannedResponse& resp,
    std::string& received_target, std::string& received_body) {
    namespace net = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using boost::asio::ip::tcp;

    tcp::socket sock = co_await acceptor.async_accept(net::use_awaitable);

    beast::flat_buffer buf;
    http::request<http::string_body> req;
    co_await http::async_read(sock, buf, req, net::use_awaitable);
    received_target = std::string(req.target());
    received_body = req.body();

    if (resp.stall) {
        // Never respond; hold the socket open so the client's read blocks.
        boost::asio::steady_timer t(sock.get_executor(), std::chrono::hours(24));
        co_await t.async_wait(net::use_awaitable);
        co_return;
    }

    if (resp.stream_chunks.empty()) {
        http::response<http::string_body> res{http::int_to_status(resp.status),
                                              req.version()};
        res.set(http::field::content_type, resp.content_type);
        res.body() = resp.body;
        res.prepare_payload();
        co_await http::async_write(sock, res, net::use_awaitable);
    } else {
        std::size_t total = 0;
        for (const auto& c : resp.stream_chunks) {
            total += c.size();
        }
        std::string head = "HTTP/1.1 " + std::to_string(resp.status) + " OK\r\n";
        head += "Content-Type: " + resp.content_type + "\r\n";
        head += "Content-Length: " + std::to_string(total) + "\r\n\r\n";
        co_await net::async_write(sock, net::buffer(head), net::use_awaitable);
        for (const auto& c : resp.stream_chunks) {
            co_await net::async_write(sock, net::buffer(c), net::use_awaitable);
        }
    }

    boost::system::error_code ec;
    sock.shutdown(tcp::socket::shutdown_send, ec);
    sock.close(ec);
    co_return;
}

}  // namespace libagent::testsupport
