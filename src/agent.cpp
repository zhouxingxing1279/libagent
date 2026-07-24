#include "libagent/agent.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <exception>
#include <iterator>
#include <utility>

namespace libagent {

Agent::Agent(AgentOptions opts) : opts_(std::move(opts)) {}

std::vector<Message> Agent::build_messages() const {
    auto history = opts_.memory->history();
    if (opts_.system_prompt) {
        std::vector<Message> messages;
        messages.reserve(history.size() + 1);
        messages.push_back({Role::System, Content{*opts_.system_prompt}});
        messages.insert(messages.end(),
                        std::make_move_iterator(history.begin()),
                        std::make_move_iterator(history.end()));
        return messages;
    }
    return history;
}

boost::asio::awaitable<ChatResponse> Agent::step() {
    ChatRequest req;
    req.options = opts_.generate;
    req.options.tools = opts_.tools->schemas();
    req.messages = build_messages();

    ChatResponse resp = co_await opts_.provider->chat(req);
    opts_.memory->add(resp.message);

    for (const auto& tc : resp.message.tool_calls) {
        opts_.memory->add(co_await execute_tool(tc));
    }
    co_return resp;
}

boost::asio::awaitable<Message> Agent::execute_tool(const ToolCall& tc) {
    Message result{Role::Tool};
    result.tool_call_id = tc.id;
    result.name = tc.name;

    const Tool* tool = opts_.tools->find(tc.name);
    if (tool == nullptr) {
        result.content.text = Json{{"error", "unknown tool: " + tc.name}}.dump();
        co_return result;
    }
    try {
        const Json out = co_await tool->handler(tc.arguments);
        result.content.text = out.dump();
    } catch (const std::exception& e) {
        result.content.text = Json{{"error", e.what()}}.dump();
    }
    co_return result;
}

boost::asio::awaitable<std::string> Agent::co_run(std::string user_input) {
    opts_.memory->add({Role::User, Content{std::move(user_input)}});

    for (int step_n = 0; step_n < opts_.max_tool_rounds; ++step_n) {
        const ChatResponse resp = co_await step();
        if (resp.message.tool_calls.empty()) {
            co_return resp.message.content.text;
        }
    }
    co_return std::string{"[libagent] max_tool_rounds reached"};
}

boost::asio::awaitable<void> Agent::co_run_stream(std::string user_input,
                                                  TokenSink sink) {
    opts_.memory->add({Role::User, Content{std::move(user_input)}});

    for (int step_n = 0; step_n < opts_.max_tool_rounds; ++step_n) {
        const ChatResponse resp = co_await step();
        if (resp.message.tool_calls.empty()) {
            if (!resp.message.content.text.empty()) {
                StreamEvent delta;
                delta.kind = StreamEvent::Kind::Delta;
                delta.delta = resp.message.content.text;
                co_await sink(delta);
            }
            StreamEvent finish;
            finish.kind = StreamEvent::Kind::Finish;
            finish.finish = resp.finish;
            finish.usage = resp.usage;
            co_await sink(finish);
            co_return;
        }
    }
    StreamEvent finish;
    finish.kind = StreamEvent::Kind::Finish;
    finish.finish = FinishReason::Error;
    co_await sink(finish);
}

std::string Agent::run(std::string user_input) {
    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(ioc, co_run(std::move(user_input)),
                                     boost::asio::use_future);
    ioc.run();
    return fut.get();
}

}  // namespace libagent
