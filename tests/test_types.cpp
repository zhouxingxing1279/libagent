#include "libagent/types.hpp"

#include <gtest/gtest.h>

namespace {

using libagent::Content;
using libagent::GenerateOptions;
using libagent::Json;
using libagent::Message;
using libagent::Role;
using libagent::ToolCall;
using libagent::Usage;

TEST(RoleHelpers, RoundTrip) {
    EXPECT_STREQ(libagent::role_to_string(Role::Tool), "tool");
    EXPECT_EQ(libagent::role_from_string("assistant"), Role::Assistant);
    EXPECT_FALSE(libagent::role_from_string("bogus").has_value());
}

TEST(Serialization, MessageRoundTrip) {
    Message m;
    m.role = Role::Assistant;
    m.content.text = "hello";
    m.tool_calls.push_back({"call_1", "get_weather", Json{{"city", "SF"}}});

    const Json j = m;
    const Message back = j.get<Message>();

    EXPECT_EQ(back.role, Role::Assistant);
    EXPECT_EQ(back.content.text, "hello");
    ASSERT_EQ(back.tool_calls.size(), 1u);
    EXPECT_EQ(back.tool_calls[0].id, "call_1");
    EXPECT_EQ(back.tool_calls[0].name, "get_weather");
    EXPECT_EQ(back.tool_calls[0].arguments["city"], "SF");
}

TEST(Serialization, ToolCallAcceptsStringArguments) {
    Json wire = Json::object();
    wire["id"] = "call_9";
    wire["name"] = "f";
    wire["arguments"] = R"({"x": 1})";  // OpenAI-style JSON string

    const ToolCall tc = wire.get<ToolCall>();
    EXPECT_EQ(tc.name, "f");
    EXPECT_EQ(tc.arguments["x"], 1);
}

TEST(Serialization, GenerateOptionsOmitsUnsetOptionals) {
    GenerateOptions g;
    g.model = "gpt-4o";

    const Json j = g;
    EXPECT_EQ(j["model"], "gpt-4o");
    EXPECT_FALSE(j.contains("temperature"));
    EXPECT_FALSE(j.contains("max_tokens"));
}

TEST(Serialization, GenerateOptionsRoundTrip) {
    GenerateOptions g;
    g.model = "gpt-4o";
    g.temperature = 0.7;
    g.tools.push_back({"t", "desc", Json::object()});

    const Json j = g;
    const GenerateOptions back = j.get<GenerateOptions>();

    ASSERT_TRUE(back.model.has_value());
    EXPECT_EQ(*back.model, "gpt-4o");
    ASSERT_TRUE(back.temperature.has_value());
    EXPECT_DOUBLE_EQ(*back.temperature, 0.7);
    ASSERT_EQ(back.tools.size(), 1u);
    EXPECT_EQ(back.tools[0].name, "t");
}

TEST(Serialization, UsageRoundTrip) {
    const Usage u{10, 20, 30};
    const Json j = u;
    const Usage back = j.get<Usage>();
    EXPECT_EQ(back.prompt_tokens, 10);
    EXPECT_EQ(back.completion_tokens, 20);
    EXPECT_EQ(back.total_tokens, 30);
}

}  // namespace
