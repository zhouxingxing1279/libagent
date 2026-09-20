#include "libagent/agent.hpp"

#include "libagent/logging.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
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

// Agent 最终需要持有整份配置，因此这里按值接收参数：调用方传左值时先拷贝，传右值时可直接移动。
// 随后通过 std::move 将局部参数中的资源转移到成员 opts_，避免一次不必要的深拷贝。
Agent::Agent(AgentOptions opts) : opts_(std::move(opts)) {}

void Agent::remember(Message m) {
    if (opts_.hooks.on_message) {
        opts_.hooks.on_message(m);
    }
    opts_.memory->add(std::move(m));
}

void Agent::do_log(LogLevel level, std::string msg) const {
    if (opts_.log) {
        opts_.log(level, msg);
    }
}

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

boost::asio::awaitable<ChatRequest> Agent::prepare_request() {
    // Let the memory do any async maintenance (e.g. summarization).
    co_await opts_.memory->compact();

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
    co_return req;
}

boost::asio::awaitable<Message> Agent::execute_tool(const ToolCall& tc) {
    const auto t0 = std::chrono::steady_clock::now();
    Message result{Role::Tool};
    result.tool_call_id = tc.id;
    result.name = tc.name;

    Json out_json;
    const Tool* tool = opts_.tools->find(tc.name);
    if (tool == nullptr) {
        out_json = Json{{"error", "unknown tool: " + tc.name}};
        result.content.text = out_json.dump();
    } else {
        try {
            out_json = co_await tool->handler(tc.arguments);
            result.content.text = out_json.dump();
        } catch (const std::exception& e) {
            out_json = Json{{"error", e.what()}};
            result.content.text = out_json.dump();
        }
    }

    if (opts_.max_tool_output_bytes != 0 &&
        result.content.text.size() > opts_.max_tool_output_bytes) {
        const std::size_t orig = result.content.text.size();
        result.content.text.resize(opts_.max_tool_output_bytes);
        result.content.text +=
            "\n...[truncated by libagent: " + std::to_string(orig) + " bytes total]";
    }

    const auto dur = std::chrono::steady_clock::now() - t0;
    if (opts_.hooks.on_tool_call) {
        opts_.hooks.on_tool_call(tc, out_json, dur);
    }
    do_log(LogLevel::Info, "tool '" + tc.name + "' completed");
    co_return result;
}

boost::asio::awaitable<void> Agent::execute_tool_calls(
    const std::vector<ToolCall>& calls) {
    if (calls.empty()) {
        co_return;
    }
    // Fan out concurrently. co_spawn(..., use_awaitable) is lazy, so to get
    // real overlap we launch each tool with detached (eager) and join via a
    // channel. On a single-threaded io_context the coroutines interleave
    // cooperatively, so IO-bound tools overlap without data races.
    auto ex = co_await boost::asio::this_coro::executor;
    auto results = std::make_shared<std::vector<Message>>(calls.size());
    using Chan = boost::asio::experimental::channel<void(boost::system::error_code,
                                                         std::size_t)>;
    auto chan = std::make_shared<Chan>(ex, calls.size());

    for (std::size_t i = 0; i < calls.size(); ++i) {
        boost::asio::co_spawn(
            ex,
            [this, &calls, i, results, chan]() -> boost::asio::awaitable<void> {
                Message m = co_await execute_tool(calls[i]);
                (*results)[i] = std::move(m);
                co_await chan->async_send(boost::system::error_code{}, i,
                                          boost::asio::use_awaitable);
            },
            boost::asio::detached);
    }

    for (std::size_t i = 0; i < calls.size(); ++i) {
        const std::size_t idx = co_await chan->async_receive(boost::asio::use_awaitable);
        remember(std::move((*results)[idx]));
    }
    co_return;
}

boost::asio::awaitable<ChatResponse> Agent::step() {
    ChatRequest req = co_await prepare_request();
    const auto t0 = std::chrono::steady_clock::now();
    ChatResponse resp = co_await opts_.provider->chat(req);
    const auto dur = std::chrono::steady_clock::now() - t0;
    if (opts_.hooks.on_llm_call) {
        opts_.hooks.on_llm_call(req, resp, dur);
    }
    do_log(LogLevel::Info,
           "llm call: finish=" + std::to_string(static_cast<int>(resp.finish)));
    remember(resp.message);
    co_await execute_tool_calls(resp.message.tool_calls);
    co_return resp;
}

boost::asio::awaitable<std::string> Agent::co_run(std::string user_input) {
    remember({Role::User, Content{std::move(user_input)}});

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
    remember({Role::User, Content{std::move(user_input)}});

    for (int step_n = 0; step_n < opts_.max_tool_rounds; ++step_n) {
        const ChatRequest req = co_await prepare_request();

        // Stream this round: forward content deltas to the caller live, and
        // reconstruct the full assistant message (content + tool_calls) from
        // the Finish event.
        Message assistant{Role::Assistant};
        FinishReason finish = FinishReason::Stop;
        Usage usage;
        bool errored = false;

        co_await opts_.provider->stream(
            req, [&](const StreamEvent& ev) -> boost::asio::awaitable<void> {
                if (ev.kind == StreamEvent::Kind::Delta) {
                    assistant.content.text += ev.delta;
                    co_await sink(ev);  // live token forwarding
                } else if (ev.kind == StreamEvent::Kind::Finish) {
                    finish = ev.finish;
                    usage = ev.usage;
                    assistant.tool_calls = ev.tool_calls;
                } else if (ev.kind == StreamEvent::Kind::Error) {
                    errored = true;
                }
                co_return;
            });

        if (errored) {
            StreamEvent f;
            f.kind = StreamEvent::Kind::Finish;
            f.finish = FinishReason::Error;
            co_await sink(f);
            co_return;
        }

        remember(assistant);

        if (assistant.tool_calls.empty()) {
            StreamEvent f;
            f.kind = StreamEvent::Kind::Finish;
            f.finish = finish;
            f.usage = usage;
            co_await sink(f);
            co_return;
        }
        co_await execute_tool_calls(assistant.tool_calls);
    }

    StreamEvent f;
    f.kind = StreamEvent::Kind::Finish;
    f.finish = FinishReason::Error;
    co_await sink(f);
}

std::string Agent::run(std::string user_input) {
    // run() 是面向普通同步调用者的适配层：内部仍复用异步核心 co_run()。
    // 这里创建一个临时 io_context，作为本次 Agent 执行的事件循环与协程调度环境。
    boost::asio::io_context ioc;

    // 将 co_run() 提交到 io_context 上执行，并使用 use_future 将协程最终结果桥接为 std::future。
    // user_input 在此后不再使用，因此通过 std::move 转交给 co_run()。
    auto fut = boost::asio::co_spawn(ioc, co_run(std::move(user_input)),
                                     boost::asio::use_future);

    // 当前线程在这里驱动整个 Asio 事件循环；LLM 请求、Tool 异步 IO 和协程恢复都依赖它向前推进。
    // 因此 run() 对外表现为阻塞式同步接口，直到本次 Agent 执行结束才继续往下执行。
    ioc.run();

    // 此时协程通常已经完成。get() 负责取出 co_run() 的最终字符串；
    // 若协程内部以异常结束，这里也会将异常重新抛回同步调用栈。
    return fut.get();
}

std::string Agent::run(std::string user_input,
                       const boost::asio::cancellation_slot& slot) {
    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(
        ioc, co_run(std::move(user_input)),
        boost::asio::bind_cancellation_slot(slot, boost::asio::use_future));
    ioc.run();
    return fut.get();
}

}  // namespace libagent
