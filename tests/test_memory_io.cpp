#include "libagent/memory.hpp"
#include "libagent/memory_io.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

using namespace libagent;

TEST(MemoryIO, RoundTripsThroughJson) {
    FullMemory m;
    m.add({Role::System, Content{"sys"}});
    m.add({Role::User, Content{"hi"}});
    m.add({Role::Assistant, Content{"hello"}});

    const Json j = serialize(m);
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 3u);

    FullMemory m2;
    load(m2, j);
    ASSERT_EQ(m2.history().size(), 3u);
    EXPECT_EQ(m2.history()[0].role, Role::System);
    EXPECT_EQ(m2.history()[0].content.text, "sys");
    EXPECT_EQ(m2.history()[1].role, Role::User);
    EXPECT_EQ(m2.history()[2].content.text, "hello");
}

TEST(MemoryIO, RoundTripsThroughFile) {
    FullMemory m;
    m.add({Role::User, Content{"persisted"}});
    m.add({Role::Assistant, Content{"ok"}});

    const auto path = std::filesystem::temp_directory_path() / "libagent_mem_test.json";
    save_to_file(m, path);

    FullMemory m2;
    load_from_file(m2, path);
    ASSERT_EQ(m2.history().size(), 2u);
    EXPECT_EQ(m2.history()[0].content.text, "persisted");
    EXPECT_EQ(m2.history()[1].role, Role::Assistant);

    std::filesystem::remove(path);
}

}  // namespace
