#include "libagent/version.h"

#include <gtest/gtest.h>

#include <cstring>

// Phase 0 smoke test: validates that the toolchain links, the generated
// version header is configured, and a LIBAGENT_API-tagged symbol resolves
// for consumers.
TEST(Version, ReportsProjectVersion) {
    ASSERT_NE(libagent::version(), nullptr);
    EXPECT_STREQ(libagent::version(), LIBAGENT_VERSION);
}

TEST(Api, ExportedSymbolResolves) {
    EXPECT_GT(std::strlen(libagent::version()), 0u);
}
