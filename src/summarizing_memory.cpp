#include "libagent/summarizing_memory.hpp"

#include <algorithm>
#include <utility>

namespace libagent {

SummarizingMemory::SummarizingMemory(Options opts, std::shared_ptr<Memory> inner)
    : opts_(std::move(opts)),
      inner_(inner ? std::move(inner) : std::make_shared<FullMemory>()) {}

void SummarizingMemory::add(Message m) { inner_->add(std::move(m)); }

std::vector<Message> SummarizingMemory::history() const { return inner_->history(); }

void SummarizingMemory::clear() { inner_->clear(); }

boost::asio::awaitable<void> SummarizingMemory::compact() {
    auto hist = inner_->history();
    if (hist.size() <= opts_.summarize_above) {
        co_return;
    }
    if (hist.size() <= opts_.keep_recent) {
        co_return;
    }

    const std::size_t keep = std::min(opts_.keep_recent, hist.size());
    const std::size_t summarize_end = hist.size() - keep;

    std::string transcript;
    for (std::size_t i = 0; i < summarize_end; ++i) {
        transcript += role_to_string(hist[i].role);
        transcript += ": ";
        transcript += hist[i].content.text;
        transcript += '\n';
    }

    ChatRequest req;
    req.options.model = opts_.model;
    req.messages.push_back({Role::System,
                            Content{"Summarize the following conversation concisely, "
                                    "preserving key facts, decisions, and any "
                                    "outstanding questions:"}});
    req.messages.push_back({Role::User, Content{std::move(transcript)}});

    ChatResponse resp = co_await opts_.provider->chat(req);
    const std::string summary = resp.message.content.text;

    inner_->clear();
    Message summary_msg{Role::System};
    summary_msg.content.text = "Summary of earlier conversation:\n" + summary;
    inner_->add(std::move(summary_msg));
    for (std::size_t i = summarize_end; i < hist.size(); ++i) {
        inner_->add(std::move(hist[i]));
    }
    co_return;
}

}  // namespace libagent
