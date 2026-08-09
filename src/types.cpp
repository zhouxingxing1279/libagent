#include "libagent/types.hpp"

#include <utility>

namespace libagent {

// ---------------------------------------------------------------------------
// Role helpers
// ---------------------------------------------------------------------------
const char* role_to_string(Role r) noexcept {
    switch (r) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

std::optional<Role> role_from_string(std::string_view s) noexcept {
    if (s == "system") return Role::System;
    if (s == "user") return Role::User;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool") return Role::Tool;
    return std::nullopt;
}

void to_json(Json& j, const Role& r) { j = role_to_string(r); }

void from_json(const Json& j, Role& r) {
    r = role_from_string(j.get<std::string>()).value_or(Role::User);
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------
void to_json(Json& j, const Content& c) { j = c.text; }

void from_json(const Json& j, Content& c) {
    if (j.is_string()) {
        c.text = j.get<std::string>();
    } else if (j.is_object() && j.contains("text") && j["text"].is_string()) {
        c.text = j["text"].get<std::string>();
    }
}

// ---------------------------------------------------------------------------
// ToolCall (arguments may arrive as a JSON string or object)
// ---------------------------------------------------------------------------
void to_json(Json& j, const ToolCall& t) {
    j = Json::object();
    j["id"] = t.id;
    j["name"] = t.name;
    j["arguments"] = t.arguments;
}

void from_json(const Json& j, ToolCall& t) {
    t.id = j.value("id", "");
    t.name = j.value("name", "");
    if (j.contains("arguments")) {
        const auto& a = j["arguments"];
        if (a.is_string()) {
            // Some APIs (OpenAI) send arguments as a JSON string.
            t.arguments = Json::parse(a.get<std::string>(), nullptr, false);
        } else {
            t.arguments = a;
        }
    }
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
void to_json(Json& j, const Message& m) {
    j = Json::object();
    j["role"] = m.role;
    j["content"] = m.content;
    if (!m.tool_calls.empty()) j["tool_calls"] = m.tool_calls;
    if (m.tool_call_id) j["tool_call_id"] = *m.tool_call_id;
    if (m.name) j["name"] = *m.name;
}

void from_json(const Json& j, Message& m) {
    if (j.contains("role")) {
        j["role"].get_to(m.role);
    } else {
        m.role = Role::User;
    }
    if (j.contains("content")) {
        j["content"].get_to(m.content);
    }
    if (j.contains("tool_calls")) {
        m.tool_calls = j["tool_calls"].get<std::vector<ToolCall>>();
    }
    if (j.contains("tool_call_id")) {
        m.tool_call_id = j["tool_call_id"].get<std::string>();
    }
    if (j.contains("name")) {
        m.name = j["name"].get<std::string>();
    }
}

// ---------------------------------------------------------------------------
// ToolDefinition
// ---------------------------------------------------------------------------
void to_json(Json& j, const ToolDefinition& t) {
    j = Json::object();
    j["name"] = t.name;
    j["description"] = t.description;
    j["parameters"] = t.parameters;
}

void from_json(const Json& j, ToolDefinition& t) {
    t.name = j.value("name", "");
    t.description = j.value("description", "");
    if (j.contains("parameters")) {
        t.parameters = j["parameters"];
    }
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
void to_json(Json& j, const Usage& u) {
    j = Json::object();
    j["prompt_tokens"] = u.prompt_tokens;
    j["completion_tokens"] = u.completion_tokens;
    j["total_tokens"] = u.total_tokens;
}

void from_json(const Json& j, Usage& u) {
    u.prompt_tokens = j.value("prompt_tokens", 0);
    u.completion_tokens = j.value("completion_tokens", 0);
    u.total_tokens = j.value("total_tokens", 0);
}

// ---------------------------------------------------------------------------
// GenerateOptions (optionals omitted when unset)
// ---------------------------------------------------------------------------
void to_json(Json& j, const GenerateOptions& g) {
    j = Json::object();
    if (g.model) j["model"] = *g.model;
    if (g.temperature) j["temperature"] = *g.temperature;
    if (g.max_tokens) j["max_tokens"] = *g.max_tokens;
    if (g.top_p) j["top_p"] = *g.top_p;
    if (!g.stop.empty()) j["stop"] = g.stop;
    if (!g.tools.empty()) j["tools"] = g.tools;  // canonical: vector<ToolDefinition>
    if (g.response_format) j["response_format"] = *g.response_format;
    if (g.tool_choice) j["tool_choice"] = *g.tool_choice;
    if (g.seed) j["seed"] = *g.seed;
    // 'stream' defaults to false and is omitted here.
}

void from_json(const Json& j, GenerateOptions& g) {
    if (j.contains("model")) g.model = j["model"].get<std::string>();
    if (j.contains("temperature")) g.temperature = j["temperature"].get<double>();
    if (j.contains("max_tokens")) g.max_tokens = j["max_tokens"].get<int>();
    if (j.contains("top_p")) g.top_p = j["top_p"].get<int>();
    if (j.contains("stop")) g.stop = j["stop"].get<std::vector<std::string>>();
    if (j.contains("tools")) g.tools = j["tools"].get<std::vector<ToolDefinition>>();
    if (j.contains("response_format")) g.response_format = j["response_format"];
    if (j.contains("tool_choice")) g.tool_choice = j["tool_choice"];
    if (j.contains("seed")) g.seed = j["seed"].get<long>();
}

}  // namespace libagent
