#include "retry.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libagent::http {

namespace {

bool is_retryable_status(unsigned status) {
    return status == 408 || status == 429 || status == 500 || status == 502 ||
           status == 503 || status == 504;
}

bool iequals_ascii(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::chrono::milliseconds> parse_retry_after(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    for (const auto& [k, v] : headers) {
        if (iequals_ascii(k, "retry-after")) {
            try {
                const long secs = std::stol(v);
                return std::chrono::milliseconds(secs * 1000);
            } catch (...) {
                return std::nullopt;  // ignore HTTP-date form / malformed values
            }
        }
    }
    return std::nullopt;
}

std::chrono::milliseconds compute_backoff(
    int attempt, std::optional<std::chrono::milliseconds> retry_after,
    std::chrono::milliseconds initial, double multiplier,
    std::chrono::milliseconds cap) {
    if (retry_after) {
        return std::min(*retry_after, cap);
    }
    const double scaled =
        static_cast<double>(initial.count()) * std::pow(multiplier, attempt);
    const auto ms = static_cast<long long>(std::round(scaled));
    return std::chrono::milliseconds(
        std::min<long long>(ms, static_cast<long long>(cap.count())));
}

boost::asio::awaitable<void> sleep_for(std::chrono::milliseconds d) {
    if (d.count() <= 0) {
        co_return;
    }
    boost::asio::steady_timer t(co_await boost::asio::this_coro::executor, d);
    co_await t.async_wait(boost::asio::use_awaitable);
}

}  // namespace

boost::asio::awaitable<Response> send_with_retry(const Request& req,
                                                 const RetryPolicy& policy) {
    HttpsClient client;
    Response resp;
    for (int attempt = 0;; ++attempt) {
        std::exception_ptr network_error;
        try {
            resp = co_await client.request(req);
        } catch (...) {
            network_error = std::current_exception();
        }
        if (network_error) {
            if (attempt >= policy.max_retries) {
                std::rethrow_exception(network_error);
            }
            co_await sleep_for(compute_backoff(attempt, std::nullopt,
                                               policy.initial_backoff, policy.multiplier,
                                               policy.max_backoff));
            continue;
        }
        if (resp.status >= 400 && is_retryable_status(resp.status) &&
            attempt < policy.max_retries) {
            co_await sleep_for(compute_backoff(attempt, parse_retry_after(resp.headers),
                                               policy.initial_backoff, policy.multiplier,
                                               policy.max_backoff));
            continue;
        }
        break;
    }
    co_return resp;
}

}  // namespace libagent::http
