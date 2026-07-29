#include "fakes/fake_provider.hpp"
#include "libagent/memory.hpp"
#include "libagent/summarizing_memory.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using namespace libagent;

void run_compact(SummarizingMemory& mem) {
    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(
        ioc, [&]() -> boost::asio::awaitable<void> { co_await mem.compact(); },
        boost::asio::use_future);
    ioc.run();
    fut.get();
}

TEST(SummarizingMemory, CompactsOldMessagesIntoSummary) {
    auto summarizer = std::make_shared<fakes::FakeProvider>();
    ChatResponse summary;
    summary.message.role = Role::Assistant;
    summary.message.content.text = "SUMMARY";
    summary.finish = FinishReason::Stop;
    summarizer->push(summary);

    SummarizingMemory::Options opts;
    opts.provider = summarizer;
    opts.summarize_above = 4;
    opts.keep_recent = 2;
    auto mem = std::make_shared<SummarizingMemory>(opts);

    for (int i = 0; i < 6; ++i) {
        mem->add({Role::User, Content{"turn " + std::to_string(i)}});
    }
    ASSERT_GT(mem->history().size(), 4u);

    run_compact(*mem);

    const auto h = mem->history();
    ASSERT_EQ(h.size(), 3u);  // 1 summary + 2 recent
    EXPECT_EQ(h[0].role, Role::System);
    EXPECT_NE(h[0].content.text.find("SUMMARY"), std::string::npos);
    EXPECT_EQ(h.back().content.text, "turn 5");
}

TEST(SummarizingMemory, DoesNotCompactUnderThreshold) {
    auto summarizer = std::make_shared<fakes::FakeProvider>();
    SummarizingMemory::Options opts;
    opts.provider = summarizer;
    opts.summarize_above = 10;
    opts.keep_recent = 4;
    auto mem = std::make_shared<SummarizingMemory>(opts);

    for (int i = 0; i < 3; ++i) {
        mem->add({Role::User, Content{"x"}});
    }
    run_compact(*mem);

    EXPECT_EQ(mem->history().size(), 3u);               // unchanged
    EXPECT_TRUE(summarizer->last_request.messages.empty());  // provider not called
}

}  // namespace
