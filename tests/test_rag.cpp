#include "fakes/fake_provider.hpp"
#include "libagent/agent.hpp"
#include "libagent/memory.hpp"
#include "libagent/retrievers/simple.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using namespace libagent;

TEST(SimpleCorpusRetriever, RanksByTokenOverlap) {
    SimpleCorpusRetriever r;
    r.add_document("the cat sat on the mat");
    r.add_document("the dog ran fast");
    r.add_document("cats and dogs are friends");

    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<std::vector<RetrievedChunk>> {
            co_return co_await r.retrieve("cat mat", 2);
        },
        boost::asio::use_future);
    ioc.run();
    const auto res = fut.get();

    ASSERT_FALSE(res.empty());
    EXPECT_EQ(res[0].text, "the cat sat on the mat");  // matches both "cat" and "mat"
    EXPECT_GT(res[0].score, 0.0f);
}

TEST(Agent, RetrieverInjectsContextIntoRequest) {
    auto provider = std::make_shared<fakes::FakeProvider>();
    ChatResponse ans;
    ans.message.role = Role::Assistant;
    ans.message.content.text = "Paris";
    ans.finish = FinishReason::Stop;
    provider->push(ans);

    auto retriever = std::make_shared<SimpleCorpusRetriever>();
    retriever->add_document("Paris is the capital of France.");

    AgentOptions opts;
    opts.provider = provider;
    opts.memory = std::make_shared<FullMemory>();
    opts.retriever = retriever;
    Agent agent(opts);

    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<std::string> {
            co_return co_await agent.co_run("What is the capital of France?");
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    bool found = false;
    for (const auto& m : provider->last_request.messages) {
        if (m.role == Role::System && m.content.text.find("Paris") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

}  // namespace
