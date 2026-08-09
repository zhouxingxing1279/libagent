#pragma once

#include "libagent/provider.hpp"

#include <chrono>
#include <string>

namespace libagent::anthropic {

struct Options {
    std::string api_key;
    /// Origin URL ("scheme://host[:port]").
    std::string base_url = "https://api.anthropic.com";
    std::string messages_path = "/v1/messages";
    std::string default_model = "claude-3-5-sonnet-20241022";
    std::string anthropic_version = "2023-06-01";

    /// Retry policy (applies to non-streaming chat()).
    int max_retries = 3;
    std::chrono::milliseconds initial_backoff{500};
    double backoff_multiplier = 2.0;
    std::chrono::milliseconds max_backoff{30000};
};

/// Anthropic Claude Messages API provider. Implements LLMProvider. Maps the
/// libagent message/tool model to/from Claude's content-block format (system as
/// a top-level field, tool_use/tool_result blocks, input_schema).
class AnthropicProvider : public LLMProvider {
public:
    explicit AnthropicProvider(Options opts);

    boost::asio::awaitable<ChatResponse> chat(const ChatRequest& req) override;
    boost::asio::awaitable<void> stream(const ChatRequest& req, TokenSink sink) override;

private:
    Options opts_;
    bool tls_ = true;
    std::string host_;
    std::string port_;
};

}  // namespace libagent::anthropic
