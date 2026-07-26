#include "fakes/fake_provider.hpp"
#include "libagent/agent.hpp"
#include "libagent/memory.hpp"
#include "libagent/orchestration.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
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

TEST(HandoffRouter, DelegatesToSingleSubAgent) {
    // Executor: answers "42" for any task.
    auto exec_provider = std::make_shared<fakes::FakeProvider>();
    PushAssistant(*exec_provider, "42", FinishReason::Stop);
    auto exec_memory = std::make_shared<FullMemory>();
    AgentOptions exec_opts;
    exec_opts.provider = exec_provider;
    exec_opts.memory = exec_memory;
    auto executor = std::make_shared<Agent>(exec_opts);

    // Router: delegates to "executor", then composes a final answer.
    auto router_provider = std::make_shared<fakes::FakeProvider>();
    PushToolCall(*router_provider, "c1", "executor",
                 Json{{"task", "compute the answer"}});
    PushAssistant(*router_provider, "The executor said: 42", FinishReason::Stop);

    HandoffRouter router(router_provider, std::make_shared<FullMemory>(),
                         {AgentHandle{"executor", executor}});

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await router.agent().co_run("what is the answer?");
    });
    EXPECT_EQ(fut.get(), "The executor said: 42");

    // The delegated task actually reached the sub-agent.
    ASSERT_FALSE(exec_memory->history().empty());
    EXPECT_EQ(exec_memory->history().front().role, Role::User);
    EXPECT_EQ(exec_memory->history().front().content.text, "compute the answer");
}

TEST(HandoffRouter, RoutesToTheRightSpecialist) {
    auto math_provider = std::make_shared<fakes::FakeProvider>();
    PushAssistant(*math_provider, "180", FinishReason::Stop);
    auto math_memory = std::make_shared<FullMemory>();
    AgentOptions math_opts;
    math_opts.provider = math_provider;
    math_opts.memory = math_memory;
    auto math_agent = std::make_shared<Agent>(math_opts);

    auto time_provider = std::make_shared<fakes::FakeProvider>();
    PushAssistant(*time_provider, "noon", FinishReason::Stop);
    auto time_memory = std::make_shared<FullMemory>();
    AgentOptions time_opts;
    time_opts.provider = time_provider;
    time_opts.memory = time_memory;
    auto time_agent = std::make_shared<Agent>(time_opts);

    auto router_provider = std::make_shared<fakes::FakeProvider>();
    PushToolCall(*router_provider, "c1", "math_agent",
                 Json{{"task", "15 * 12"}});
    PushAssistant(*router_provider, "The product is 180", FinishReason::Stop);

    HandoffRouter router(
        router_provider, std::make_shared<FullMemory>(),
        {AgentHandle{"math_agent", math_agent}, AgentHandle{"time_agent", time_agent}});

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await router.agent().co_run("what is 15 * 12?");
    });
    EXPECT_EQ(fut.get(), "The product is 180");

    // Only the math specialist was consulted.
    EXPECT_FALSE(math_memory->history().empty());
    EXPECT_TRUE(time_memory->history().empty());
}

TEST(HandoffRouter, AddAgentAtRuntime) {
    auto exec_provider = std::make_shared<fakes::FakeProvider>();
    PushAssistant(*exec_provider, "ok", FinishReason::Stop);
    auto exec_memory = std::make_shared<FullMemory>();
    AgentOptions exec_opts;
    exec_opts.provider = exec_provider;
    exec_opts.memory = exec_memory;
    auto executor = std::make_shared<Agent>(exec_opts);

    auto router_provider = std::make_shared<fakes::FakeProvider>();
    PushToolCall(*router_provider, "c1", "late", Json{{"task", "hi"}});
    PushAssistant(*router_provider, "done", FinishReason::Stop);

    HandoffRouter router(router_provider, std::make_shared<FullMemory>(), {});
    router.add_agent({"late", executor});

    AgentRunner runner;
    auto fut = runner.run([&]() -> awaitable<std::string> {
        co_return co_await router.agent().co_run("delegate");
    });
    EXPECT_EQ(fut.get(), "done");
    EXPECT_FALSE(exec_memory->history().empty());
}

}  // namespace
