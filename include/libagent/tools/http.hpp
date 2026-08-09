#pragma once

#include "libagent/tool.hpp"

#include <chrono>
#include <cstddef>
#include <string>

namespace libagent::tools {

struct HttpGetOptions {
    std::chrono::steady_clock::duration timeout = std::chrono::seconds(30);
    /// Truncate the response body to this many bytes (0 = unlimited).
    std::size_t max_body_bytes = 20000;
};

/// Build a Tool that performs an HTTP GET on a `{url}` argument and returns
/// `{"status": <int>, "body": <string>}` (or `{"error": ...}` on failure).
/// Native (uses the embedded HTTPS client), so no subprocess is spawned.
Tool http_get(std::string name, std::string description, HttpGetOptions opts = {});

}  // namespace libagent::tools
