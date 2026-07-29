#pragma once

#include "libagent/memory.hpp"
#include "libagent/provider.hpp"
#include "libagent/types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace libagent {

/// Memory that summarizes older conversation turns once the transcript grows
/// past a threshold. Wraps an inner memory (defaults to FullMemory); when
/// `compact()` is called (by the Agent each step) and history exceeds
/// `summarize_above`, everything except the most recent `keep_recent` messages
/// is replaced by a single System summary produced by the provider.
class SummarizingMemory : public Memory {
public:
    struct Options {
        std::shared_ptr<LLMProvider> provider;  // used to summarize
        std::size_t summarize_above = 12;       // compact when history exceeds this
        std::size_t keep_recent = 6;            // messages kept verbatim
        std::optional<std::string> model;       // optional model override
    };

    SummarizingMemory(Options opts, std::shared_ptr<Memory> inner = nullptr);

    void add(Message m) override;
    [[nodiscard]] std::vector<Message> history() const override;
    void clear() override;

    boost::asio::awaitable<void> compact() override;

private:
    Options opts_;
    std::shared_ptr<Memory> inner_;
};

}  // namespace libagent
