#pragma once

#include "libagent/provider.hpp"

#include <queue>
#include <utility>

namespace libagent::fakes {

/// In-memory LLMProvider that returns scripted ChatResponses from a FIFO
/// queue. No network — drives agent/provider tests deterministically.
class FakeProvider : public LLMProvider {
public:
    /// Most recent request seen by chat() (for assertions).
    ChatRequest last_request;

    void push(ChatResponse r) { responses_.push(std::move(r)); }

    [[nodiscard]] bool empty() const { return responses_.empty(); }

    boost::asio::awaitable<ChatResponse> chat(const ChatRequest& req) override {
        last_request = req;
        co_return pop_or_default();
    }

    boost::asio::awaitable<void> stream(const ChatRequest& /*req*/,
                                        TokenSink sink) override {
        const ChatResponse r = pop_or_default();

        if (!r.message.content.text.empty()) {
            StreamEvent delta;
            delta.kind = StreamEvent::Kind::Delta;
            delta.delta = r.message.content.text;
            co_await sink(delta);
        }

        StreamEvent finish;
        finish.kind = StreamEvent::Kind::Finish;
        finish.finish = r.finish;
        finish.usage = r.usage;
        finish.tool_calls = r.message.tool_calls;
        co_await sink(finish);
    }

private:
    ChatResponse pop_or_default() {
        if (responses_.empty()) {
            ChatResponse r;
            r.message.role = Role::Assistant;
            r.finish = FinishReason::Stop;
            return r;
        }
        ChatResponse r = std::move(responses_.front());
        responses_.pop();
        return r;
    }

    std::queue<ChatResponse> responses_;
};

}  // namespace libagent::testing
