#pragma once

#include "libagent/types.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/system/error_code.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace libagent {

/// An asynchronous message bus for inter-agent communication. Each registered
/// agent gets a buffered FIFO of Messages (backed by asio::experimental::channel).
/// `post` enqueues a message to a named agent; `await_message` dequeues; `close`
/// shuts down an agent's channel so pending awaiters complete with an error.
class AgentBus {
public:
    explicit AgentBus(boost::asio::any_io_executor executor)
        : executor_(std::move(executor)) {}

    /// Create a channel for a named agent. Idempotent.
    void register_agent(const std::string& name) {
        if (channels_.find(name) == channels_.end()) {
            channels_.emplace(name, std::make_shared<Channel>(executor_, kCapacity));
        }
    }

    /// Send a message to a named agent (buffers up to the channel capacity).
    boost::asio::awaitable<void> post(const std::string& to, Message msg) {
        const auto it = channels_.find(to);
        if (it == channels_.end()) {
            throw std::runtime_error("libagent: AgentBus: unknown agent '" + to + "'");
        }
        co_await it->second->async_send(boost::system::error_code{}, std::move(msg),
                                        boost::asio::use_awaitable);
    }

    /// Await the next message for a named agent. Throws on channel close.
    boost::asio::awaitable<Message> await_message(const std::string& name) {
        const auto it = channels_.find(name);
        if (it == channels_.end()) {
            throw std::runtime_error("libagent: AgentBus: unknown agent '" + name + "'");
        }
        co_return co_await it->second->async_receive(boost::asio::use_awaitable);
    }

    /// Close an agent's channel; its pending/next await_message completes with
    /// an error.
    void close(const std::string& name) {
        const auto it = channels_.find(name);
        if (it != channels_.end()) {
            it->second->close();
        }
    }

private:
    using Channel =
        boost::asio::experimental::channel<void(boost::system::error_code, Message)>;

    static constexpr std::size_t kCapacity = 64;

    boost::asio::any_io_executor executor_;
    std::unordered_map<std::string, std::shared_ptr<Channel>> channels_;
};

}  // namespace libagent
