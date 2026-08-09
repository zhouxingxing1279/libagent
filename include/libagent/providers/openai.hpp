#pragma once

#include "libagent/provider.hpp"

#include <chrono>
#include <string>

namespace libagent::openai {

struct Options {
    std::string api_key;
    /// Origin URL ("scheme://host[:port]"). Configure for OpenAI-compatible
    /// backends: DeepSeek, Zhipu GLM, Moonshot, vLLM, Ollama, etc.
    std::string base_url = "https://api.openai.com";
    std::string chat_path = "/v1/chat/completions";
    std::string default_model = "gpt-4o-mini";
    bool verify_tls = true;

    /// Retry policy (applies to non-streaming chat()). Transient errors
    /// (timeout / connection / HTTP 408/429/5xx) are retried with exponential
    /// backoff; a `Retry-After` header overrides the computed delay.
    int max_retries = 3;
    std::chrono::milliseconds initial_backoff{500};
    double backoff_multiplier = 2.0;
    std::chrono::milliseconds max_backoff{30000};
};

/// OpenAI-compatible Chat Completions provider. Implements LLMProvider.
class OpenAiProvider : public LLMProvider {
public:
    explicit OpenAiProvider(Options opts);

    boost::asio::awaitable<ChatResponse> chat(const ChatRequest& req) override;
    boost::asio::awaitable<void> stream(const ChatRequest& req, TokenSink sink) override;

private:
    Options opts_;
    bool tls_ = true;
    std::string host_;
    std::string port_;
};

}  // namespace libagent::openai
