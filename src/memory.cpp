#include "libagent/memory.hpp"

#include <algorithm>
#include <utility>

namespace libagent {

// ---------------------------------------------------------------------------
// FullMemory
// ---------------------------------------------------------------------------
void FullMemory::add(Message m) { msgs_.push_back(std::move(m)); }

std::vector<Message> FullMemory::history() const { return msgs_; }

void FullMemory::clear() { msgs_.clear(); }

// ---------------------------------------------------------------------------
// WindowMemory
// ---------------------------------------------------------------------------
WindowMemory::WindowMemory(std::size_t max_messages) : max_(max_messages) {
    if (max_ == 0) {
        max_ = 1;  // always keep at least the most recent message
    }
}

void WindowMemory::add(Message m) {
    msgs_.push_back(std::move(m));

    while (msgs_.size() > max_) {
        const auto it = std::find_if(
            msgs_.begin(), msgs_.end(),
            [](const Message& x) { return x.role != Role::System; });
        if (it == msgs_.end()) {
            break;  // only system messages left; stop evicting
        }
        msgs_.erase(it);
    }
}

std::vector<Message> WindowMemory::history() const { return msgs_; }

void WindowMemory::clear() { msgs_.clear(); }

}  // namespace libagent
