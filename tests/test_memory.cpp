#include "libagent/memory.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace libagent;

TEST(WindowMemory, EvictsOldestKeepingSystemPrompt) {
    WindowMemory mem(3);
    mem.add({Role::System, Content{"sys"}});
    mem.add({Role::User, Content{"u1"}});
    mem.add({Role::User, Content{"u2"}});
    mem.add({Role::User, Content{"u3"}});
    mem.add({Role::User, Content{"u4"}});  // over capacity twice

    const auto h = mem.history();
    ASSERT_EQ(h.size(), 3u);
    EXPECT_EQ(h.front().role, Role::System);
    EXPECT_EQ(h.front().content.text, "sys");
    EXPECT_EQ(h.back().content.text, "u4");
}

TEST(WindowMemory, PreservesSystemWhenOnlySystemRemains) {
    WindowMemory mem(2);
    mem.add({Role::System, Content{"sys"}});
    mem.add({Role::User, Content{"u1"}});
    mem.add({Role::User, Content{"u2"}});

    const auto h = mem.history();
    EXPECT_EQ(h.front().role, Role::System);
}

TEST(FullMemory, KeepsEverythingAndClears) {
    FullMemory mem;
    for (int i = 0; i < 5; ++i) {
        mem.add({Role::User, Content{std::to_string(i)}});
    }
    EXPECT_EQ(mem.history().size(), 5u);
    mem.clear();
    EXPECT_TRUE(mem.history().empty());
}

}  // namespace
