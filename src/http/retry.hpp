#pragma once

#include "https_client.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>

namespace libagent::http {

/// Retry policy for HTTP requests. Shared across providers.
struct RetryPolicy {
    int max_retries = 3;
    std::chrono::milliseconds initial_backoff{500};
    double multiplier = 2.0;
    std::chrono::milliseconds max_backoff{30000};
};

/// POST `req` with retry on transient network errors and retryable HTTP status
/// (408/429/5xx). Honors a `Retry-After` header when present, else exponential
/// backoff. Returns the final response (which may carry an error status).
boost::asio::awaitable<Response> send_with_retry(const Request& req, const RetryPolicy& policy);

}  // namespace libagent::http
