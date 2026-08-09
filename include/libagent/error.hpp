#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace libagent {

/// Typed error categories. Throw/caught as `libagent::Error` (which IS-A
/// std::runtime_error, so existing `catch (const std::runtime_error&)` keeps
/// working) so callers can branch on the failure mode.
enum class ErrorCode {
    Timeout,       ///< A network operation exceeded its deadline.
    Cancelled,     ///< (Reserved; cancellation currently surfaces as asio
                   ///<  operation_aborted / system_error, not Error.)
    Network,       ///< Connection / DNS / reset / other transport failure.
    Auth,          ///< HTTP 401 / 403.
    RateLimited,   ///< HTTP 429.
    Http,          ///< Other HTTP error status (see http_status()).
    Provider,      ///< Malformed or unusable provider response.
    Tool,          ///< (Reserved; tool errors are returned as {"error":...}
                   ///<  messages to the model, not thrown.)
};

/// A typed libagent error. Inherits std::runtime_error for backward
/// compatibility with code that catches runtime_error / std::exception.
class Error : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message, int http_status = 0)
        : std::runtime_error(std::move(message)),
          code_(code),
          http_status_(http_status) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] int http_status() const noexcept { return http_status_; }

private:
    ErrorCode code_;
    int http_status_;
};

}  // namespace libagent
