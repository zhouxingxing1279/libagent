#pragma once

#include "libagent/types.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <vector>

namespace libagent {

/// Conversation memory. Pluggable so backends (summarization, vector store)
/// can be added later.
class Memory {
public:
    virtual ~Memory() = default;
    virtual void add(Message m) = 0;
    [[nodiscard]] virtual std::vector<Message> history() const = 0;
    virtual void clear() = 0;

    /// Optional async maintenance (e.g. summarization). Default is a no-op;
    /// called by the Agent before each step so only memories that need it do
    /// any work.
    virtual boost::asio::awaitable<void> compact() { co_return; }
};

/// Full transcript, no truncation.
class FullMemory : public Memory {
public:
    void add(Message m) override;
    [[nodiscard]] std::vector<Message> history() const override;
    void clear() override;

private:
    std::vector<Message> msgs_;
};

/// Fixed-size window. When over capacity, evicts the oldest *non-System*
/// message, always preserving the system prompt.
class WindowMemory : public Memory {
public:
    explicit WindowMemory(std::size_t max_messages);

    void add(Message m) override;
    [[nodiscard]] std::vector<Message> history() const override;
    void clear() override;

private:
    std::vector<Message> msgs_;
    std::size_t max_;
};

}  // namespace libagent
