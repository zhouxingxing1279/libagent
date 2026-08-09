#pragma once

#include "libagent/memory.hpp"
#include "libagent/provider.hpp"
#include "libagent/retriever.hpp"
#include "libagent/streaming.hpp"
#include "libagent/tool.hpp"
#include "libagent/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libagent {

struct AgentOptions {
    std::shared_ptr<LLMProvider> provider;
    std::shared_ptr<Memory> memory;
    std::shared_ptr<ToolRegistry> tools = std::make_shared<ToolRegistry>();

    /// Injected as a leading System message on every request (not stored in
    /// memory). Leave unset if you manage the system prompt via memory yourself.
    std::optional<std::string> system_prompt;

    /// Optional RAG retriever. When set, the agent retrieves `rag_top_k`
    /// chunks for the latest user query each step and injects them as context.
    std::shared_ptr<Retriever> retriever;
    int rag_top_k = 3;

    GenerateOptions generate;
    int max_tool_rounds = 10;  ///< ReAct safety bound.
};

/// An agent runs a ReAct loop: prompt the provider, and whenever the model
/// emits tool_calls, execute them, feed the results back, and repeat until the
/// model produces a final answer (or max_tool_rounds is hit).
class Agent {
public:
    explicit Agent(AgentOptions opts);

    /// Coroutine entry point. Run on the caller's io_context, e.g.:
    ///   co_spawn(ioc, agent.co_run(input), use_future).get();
    boost::asio::awaitable<std::string> co_run(std::string user_input);

    /// Sink-based variant: delivers the final answer as a Delta event followed
    /// by a single Finish event. Tool rounds are non-streamed (they need the
    /// full assistant message to parse tool_calls); only the final answer is
    /// pushed through the sink. Live per-token streaming of that final segment
    /// requires provider-side tool-call streaming and is a future enhancement.
    boost::asio::awaitable<void> co_run_stream(std::string user_input, TokenSink sink);

    /// Blocking convenience: runs co_run on an internal io_context.
    std::string run(std::string user_input);

    /// Like run(), but bound to a cancellation slot so the caller can abort
    /// the run (from another thread) via the slot's cancellation_signal.
    std::string run(std::string user_input, const boost::asio::cancellation_slot& slot);

private:
    /// One ReAct step: call the provider, persist the assistant message, and if
    /// it requested tools, execute them and persist their results. Returns the
    /// assistant response so the caller can decide whether to terminate.
    boost::asio::awaitable<ChatRequest> prepare_request();
    boost::asio::awaitable<ChatResponse> step();

    boost::asio::awaitable<Message> execute_tool(const ToolCall& tc);
    boost::asio::awaitable<void> execute_tool_calls(const std::vector<ToolCall>& calls);

    std::vector<Message> build_messages(const std::string& context) const;
    std::string latest_user_query() const;

    AgentOptions opts_;
};

}  // namespace libagent
