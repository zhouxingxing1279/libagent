#include "libagent/tool.hpp"

#include <boost/asio/awaitable.hpp>

#include <gtest/gtest.h>

namespace {

using namespace libagent;

boost::asio::awaitable<Json> echo_handler(const Json& args) {
    co_return args;
}

TEST(ToolRegistry, AddFindRemove) {
    ToolRegistry reg;
    Tool t;
    t.spec.name = "get_weather";
    t.spec.description = "Get the weather";
    t.spec.parameters = Json::object({{"type", "object"}});
    t.handler = echo_handler;
    reg.add(std::move(t));

    ASSERT_NE(reg.find("get_weather"), nullptr);
    EXPECT_EQ(reg.find("missing"), nullptr);
    EXPECT_EQ(reg.size(), 1u);

    EXPECT_TRUE(reg.remove("get_weather"));
    EXPECT_FALSE(reg.remove("get_weather"));
    EXPECT_TRUE(reg.empty());
}

TEST(ToolRegistry, SchemasAndProviderSchema) {
    ToolRegistry reg;
    Tool t;
    t.spec.name = "echo";
    t.spec.description = "echo back";
    t.spec.parameters = Json::object({{"type", "object"}});
    t.handler = echo_handler;
    reg.add(std::move(t));

    const auto defs = reg.schemas();
    ASSERT_EQ(defs.size(), 1u);
    EXPECT_EQ(defs[0].name, "echo");

    const Json ps = reg.to_provider_schema();
    ASSERT_EQ(ps.size(), 1u);
    EXPECT_EQ(ps[0]["name"], "echo");
    EXPECT_EQ(ps[0]["description"], "echo back");
    EXPECT_EQ(ps[0]["parameters"]["type"], "object");
}

}  // namespace
