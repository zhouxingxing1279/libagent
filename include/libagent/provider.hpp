#pragma once

#include "libagent/streaming.hpp"
#include "libagent/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <vector>

namespace libagent {

struct ChatRequest {
    std::vector<Message> messages;
    GenerateOptions options;
};

struct ChatResponse {
    Message message;
    FinishReason finish = FinishReason::Stop;
    Usage usage;
    std::optional<Json> raw;  // optional raw provider payload for debugging
};

/// Abstract LLM provider. The architectural keystone of libagent.
///
/// All operations return boost::asio::awaitable<T> and run on the caller's
/// io_context. The provider keeps request/response buffers alive in its
/// coroutine frame for the duration of the await.
class LLMProvider {
public:
    virtual ~LLMProvider() = default;

    LLMProvider() = default;
    LLMProvider(LLMProvider&&) = default;
    LLMProvider& operator=(LLMProvider&&) = default;
    LLMProvider(const LLMProvider&) = delete;
    LLMProvider& operator=(const LLMProvider&) = delete;

    /// One-shot chat completion.
    virtual boost::asio::awaitable<ChatResponse> chat(const ChatRequest& req) = 0;

    /// Streaming completion: pushes events to `sink`, terminating with a
    /// Finish (or Error) event. The final fields live on the Finish event.
    virtual boost::asio::awaitable<void> stream(const ChatRequest& req,
                                                TokenSink sink) = 0;
};

}  // namespace libagent
