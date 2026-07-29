#include "libagent/agent.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <exception>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace libagent {

namespace {

std::string join_context(const std::vector<RetrievedChunk>& chunks) {
    std::string out;
    for (const auto& c : chunks) {
        out += "- ";
        out += c.text;
        out += '\n';
    }
    return out;
}

}  // namespace

Agent::Agent(AgentOptions opts) : opts_(std::move(opts)) {}

std::vector<Message> Agent::build_messages(const std::string& context) const {
    auto history = opts_.memory->history();

    std::string sys;
    if (opts_.system_prompt) {
        sys = *opts_.system_prompt;
    }
    if (!context.empty()) {
        if (!sys.empty()) {
            sys += "\n\n";
        }
        sys += "Relevant context from retrieved documents:\n" + context;
    }

    std::vector<Message> messages;
    messages.reserve(history.size() + 1);
    if (!sys.empty()) {
        messages.push_back({Role::System, Content{std::move(sys)}});
    }
    messages.insert(messages.end(),
                    std::make_move_iterator(history.begin()),
                    std::make_move_iterator(history.end()));
    return messages;
}

std::string Agent::latest_user_query() const {
    const auto history = opts_.memory->history();
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->role == Role::User) {
            return it->content.text;
        }
    }
    return {};
}

boost::asio::awaitable<ChatResponse> Agent::step() {
    // Let the memory do any async maintenance (e.g. summarization).
    co_await opts_.memory->compact();

    // Optional RAG: retrieve context for the latest user query.
    std::string context;
    if (opts_.retriever) {
        const std::string query = latest_user_query();
        if (!query.empty()) {
            auto chunks = co_await opts_.retriever->retrieve(query, opts_.rag_top_k);
            context = join_context(chunks);
        }
    }

    ChatRequest req;
    req.options = opts_.generate;
    req.options.tools = opts_.tools->schemas();
    req.messages = build_messages(context);

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
