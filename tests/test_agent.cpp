#include "fakes/fake_provider.hpp"
#include "libagent/agent.hpp"
#include "libagent/memory.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using namespace libagent;
using boost::asio::awaitable;

struct AgentRunner {
    boost::asio::io_context ioc;

    template <class F>
    auto run(F&& f) {
        auto fut = boost::asio::co_spawn(ioc, std::forward<F>(f),
                                         boost::asio::use_future);
        ioc.run();
        return fut;
    }
};

// Helper: push an assistant response onto a FakeProvider.
void PushAssistant(fakes::FakeProvider& p, std::string text, FinishReason fr) {
    ChatResponse r;
    r.message.role = Role::Assistant;
    r.message.content.text = std::move(text);
    r.finish = fr;
    p.push(r);
}

void PushToolCall(fakes::FakeProvider& p, std::string id, std::string name,
                  Json args) {
    ChatResponse r;
    r.message.role = Role::Assistant;
    r.finish = FinishReason::ToolCalls;
    r.message.tool_calls.push_back({std::move(id), std::move(name), std::move(args)});
    p.push(r);
}

Tool echo_tool(std::string name) {
    Tool t;
    t.spec.name = std::move(name);
    t.spec.description = "echo";
    t.spec.parameters = Json::object();
    t.handler = [](const Json& args) -> awaitable<Json> { co_return args; };
    return t;
}

TEST(Agent, DirectAnswerNoTools) {
    auto provider = std::make_shared<fakes::FakeProvider>();
    PushAssistant(*provider, "Hi there", FinishReason::Stop);

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    Agent agent(opts);

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await agent.co_run("hello");
    });
    EXPECT_EQ(fut.get(), "Hi there");
}

TEST(Agent, SingleToolRound) {
    auto provider = std::make_shared<fakes::FakeProvider>();
    PushToolCall(*provider, "call_1", "get_weather", Json{{"city", "SF"}});
    PushAssistant(*provider, "It is 72 in SF", FinishReason::Stop);

    Json captured;
    auto tools = std::make_shared<ToolRegistry>();
    Tool t;
    t.spec.name = "get_weather";
    t.spec.description = "weather";
    t.spec.parameters = Json::object();
    t.handler = [&captured](const Json& args) -> awaitable<Json> {
        captured = args;
        co_return Json{{"temp", 72}};
    };
    tools->add(std::move(t));

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    opts.tools = tools;
    Agent agent(opts);

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await agent.co_run("weather?");
    });
    EXPECT_EQ(fut.get(), "It is 72 in SF");
    EXPECT_EQ(captured["city"], "SF");
}

TEST(Agent, MultiRoundToolChain) {
    auto provider = std::make_shared<fakes::FakeProvider>();
    PushToolCall(*provider, "a", "step_a", Json::object());
    PushToolCall(*provider, "b", "step_b", Json::object());
    PushAssistant(*provider, "done after two tools", FinishReason::Stop);

    int calls = 0;
    auto tools = std::make_shared<ToolRegistry>();
    tools->add(echo_tool("step_a"));
    tools->add(echo_tool("step_b"));
    // Track call count via an instrumented copy of the registry: count by
    // wrapping each handler is overkill; instead assert final text below.

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    opts.tools = tools;
    Agent agent(opts);

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await agent.co_run("go");
    });
    EXPECT_EQ(fut.get(), "done after two tools");
    (void)calls;
}

TEST(Agent, ToolErrorBecomesMessageAndContinues) {
    auto provider = std::make_shared<fakes::FakeProvider>();
    PushToolCall(*provider, "c", "boom", Json::object());
    PushAssistant(*provider, "sorry, the tool failed", FinishReason::Stop);

    auto tools = std::make_shared<ToolRegistry>();
    Tool t;
    t.spec.name = "boom";
    t.spec.description = "always fails";
    t.spec.parameters = Json::object();
    t.handler = [](const Json&) -> awaitable<Json> {
        throw std::runtime_error("kaboom");
    };
    tools->add(std::move(t));

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    opts.tools = tools;
    Agent agent(opts);

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await agent.co_run("use the tool");
    });

    EXPECT_EQ(fut.get(), "sorry, the tool failed");
    // The failing tool call produced a Role::Tool error message in memory.
    const auto hist = opts.memory->history();
    bool saw_error = false;
    for (const auto& m : hist) {
        if (m.role == Role::Tool && m.content.text.find("error") != std::string::npos) {
            saw_error = true;
        }
    }
    EXPECT_TRUE(saw_error);
}

TEST(Agent, MaxToolRoundsReached) {
    auto provider = std::make_shared<fakes::FakeProvider>();
    for (int i = 0; i < 3; ++i) {
        PushToolCall(*provider, "c", "loop", Json::object());
    }

    auto tools = std::make_shared<ToolRegistry>();
    tools->add(echo_tool("loop"));

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    opts.tools = tools;
    opts.max_tool_rounds = 3;
    Agent agent(opts);

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await agent.co_run("loop forever");
    });
    EXPECT_EQ(fut.get(), "[libagent] max_tool_rounds reached");
}

TEST(Agent, StreamDeliversFinalAnswer) {
    auto provider = std::make_shared<fakes::FakeProvider>();
    PushAssistant(*provider, "final answer", FinishReason::Stop);

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    Agent agent(opts);

    std::string assembled;
    FinishReason finish = FinishReason::Error;
    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<void> {
        TokenSink sink = [&](const StreamEvent& ev) -> awaitable<void> {
            if (ev.kind == StreamEvent::Kind::Delta) {
                assembled += ev.delta;
            } else if (ev.kind == StreamEvent::Kind::Finish) {
                finish = ev.finish;
            }
            co_return;
        };
        co_await agent.co_run_stream("hi", sink);
    });
    fut.get();

    EXPECT_EQ(assembled, "final answer");
    EXPECT_EQ(finish, FinishReason::Stop);
}

TEST(Agent, ToolCallsRunConcurrently) {
    // A SINGLE round with two tool calls => real fan-out. Tool "a" yields
    // mid-flight; with concurrent execution "b" runs during a's suspension, so
    // a observes b having started. (Sequential execution would leave a_saw_b
    // false.)
    auto provider = std::make_shared<fakes::FakeProvider>();
    ChatResponse round1;
    round1.message.role = Role::Assistant;
    round1.finish = FinishReason::ToolCalls;
    round1.message.tool_calls.push_back({"1", "a", Json::object()});
    round1.message.tool_calls.push_back({"2", "b", Json::object()});
    provider->push(round1);
    PushAssistant(*provider, "done", FinishReason::Stop);

    struct State {
        bool a_started = false;
        bool b_started = false;
        bool a_saw_b = false;
    };
    auto st = std::make_shared<State>();

    auto tools = std::make_shared<ToolRegistry>();
    Tool ta;
    ta.spec.name = "a";
    ta.spec.parameters = Json::object();
    ta.handler = [st](const Json&) -> awaitable<Json> {
        st->a_started = true;
        auto ex = co_await boost::asio::this_coro::executor;
        boost::asio::steady_timer t(ex, std::chrono::milliseconds(20));
        co_await t.async_wait(boost::asio::use_awaitable);  // suspend + yield
        st->a_saw_b = st->b_started;
        co_return Json::object();
    };
    tools->add(std::move(ta));
    Tool tb;
    tb.spec.name = "b";
    tb.spec.parameters = Json::object();
    tb.handler = [st](const Json&) -> awaitable<Json> {
        st->b_started = true;
        co_return Json::object();
    };
    tools->add(std::move(tb));

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    opts.tools = tools;
    Agent agent(opts);

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await agent.co_run("go");
    });
    fut.get();

    EXPECT_TRUE(st->a_started);
    EXPECT_TRUE(st->b_started);
    EXPECT_TRUE(st->a_saw_b);  // proves cooperative concurrency
}

TEST(Agent, StreamHandlesToolRoundThenFinal) {
    // Round 1 streams a tool call (no content); round 2 streams the final text.
    auto provider = std::make_shared<fakes::FakeProvider>();
    PushToolCall(*provider, "c1", "echo_tool", Json::object());
    PushAssistant(*provider, "final streamed answer", FinishReason::Stop);

    auto tools = std::make_shared<ToolRegistry>();
    tools->add(echo_tool("echo_tool"));

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    opts.tools = tools;
    Agent agent(opts);

    std::string assembled;
    FinishReason finish = FinishReason::Error;
    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<void> {
        TokenSink sink = [&](const StreamEvent& ev) -> awaitable<void> {
            if (ev.kind == StreamEvent::Kind::Delta) {
                assembled += ev.delta;
            } else if (ev.kind == StreamEvent::Kind::Finish) {
                finish = ev.finish;
            }
            co_return;
        };
        co_await agent.co_run_stream("hi", sink);
    });
    fut.get();

    EXPECT_EQ(assembled, "final streamed answer");
    EXPECT_EQ(finish, FinishReason::Stop);
    bool ran_tool = false;
    for (const auto& m : opts.memory->history()) {
        if (m.role == Role::Tool) {
            ran_tool = true;
        }
    }
    EXPECT_TRUE(ran_tool);
}

// A provider whose chat() blocks on a long, cancellable await — stands in for
// a slow/in-flight network call so we can exercise cancellation.
class HangingProvider : public LLMProvider {
public:
    boost::asio::awaitable<ChatResponse> chat(const ChatRequest&) override {
        boost::asio::steady_timer t(co_await boost::asio::this_coro::executor,
                                    std::chrono::seconds(30));
        co_await t.async_wait(boost::asio::use_awaitable);
        ChatResponse r;
        r.message.content.text = "should not reach";
        co_return r;
    }
    boost::asio::awaitable<void> stream(const ChatRequest&, TokenSink) override {
        co_return;
    }
};

TEST(Agent, RunIsCancellableViaCancellationSlot) {
    auto provider = std::make_shared<HangingProvider>();
    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    Agent agent(opts);

    boost::asio::io_context ioc;
    boost::asio::cancellation_signal sig;
    auto fut = boost::asio::co_spawn(
        ioc, agent.co_run("hi"),
        boost::asio::bind_cancellation_slot(sig.slot(), boost::asio::use_future));

    // Emit the cancellation from a timer on the same io_context shortly after.
    boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<void> {
            boost::asio::steady_timer t(co_await boost::asio::this_coro::executor,
                                        std::chrono::milliseconds(20));
            co_await t.async_wait(boost::asio::use_awaitable);
            sig.emit(boost::asio::cancellation_type::all);
            co_return;
        },
        boost::asio::detached);
    ioc.run();

    EXPECT_THROW({ (void)fut.get(); }, boost::system::system_error);
}

TEST(Agent, HooksFireOnMessageLlmAndTool) {
    auto provider = std::make_shared<fakes::FakeProvider>();
    PushToolCall(*provider, "c1", "echo_tool", Json::object());
    PushAssistant(*provider, "done", FinishReason::Stop);

    auto tools = std::make_shared<ToolRegistry>();
    tools->add(echo_tool("echo_tool"));

    int messages = 0;
    int llm_calls = 0;
    int tool_calls = 0;
    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    opts.tools = tools;
    opts.hooks.on_message = [&](const Message&) { ++messages; };
    opts.hooks.on_llm_call =
        [&](const ChatRequest&, const ChatResponse&, std::chrono::steady_clock::duration) {
            ++llm_calls;
        };
    opts.hooks.on_tool_call =
        [&](const ToolCall&, const Json&, std::chrono::steady_clock::duration) { ++tool_calls; };
    Agent agent(opts);

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await agent.co_run("go");
    });
    EXPECT_EQ(fut.get(), "done");

    // user, assistant(toolcall), tool result, assistant(final)
    EXPECT_EQ(messages, 4);
    EXPECT_EQ(llm_calls, 2);
    EXPECT_EQ(tool_calls, 1);
}

}  // namespace
