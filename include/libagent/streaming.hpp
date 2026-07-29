#pragma once

#include "libagent/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <functional>
#include <string>

namespace libagent {

/// Incremental streaming event pushed by a provider during a streaming chat.
/// Kept self-contained (no ChatResponse dependency) so this header has no
/// include cycle with provider.hpp.
struct StreamEvent {
    enum class Kind { Delta, ToolCallDelta, Finish, Error };

    Kind kind = Kind::Delta;

    std::string delta;                // Kind::Delta: incremental text
    std::string tool_call_name;       // Kind::ToolCallDelta: tool being built
    std::string tool_call_arguments;  // Kind::ToolCallDelta: partial JSON args

    FinishReason finish = FinishReason::Stop;  // Kind::Finish
    Usage usage;                               // Kind::Finish
    std::vector<ToolCall> tool_calls;          // Kind::Finish: parsed tool calls

    std::string error;                // Kind::Error
};

/// The sink is itself awaitable so a slow consumer exerts real backpressure:
/// the provider coroutine suspends until the consumer's co_await resumes.
using TokenSink = std::function<boost::asio::awaitable<void>(const StreamEvent&)>;

}  // namespace libagent
