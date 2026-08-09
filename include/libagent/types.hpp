#pragma once

#include "libagent/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libagent {

// ---------------------------------------------------------------------------
// Core value types. Plain aggregates, no virtual functions. They carry a
// canonical JSON serialization (declared here, defined in types.cpp) which is
// used for persistence/tests; each provider maps these to its own wire format.
// ---------------------------------------------------------------------------

enum class Role { System, User, Assistant, Tool };

/// A reference to an image attached to a message. `url` is used for OpenAI
/// image_url (and may be a data: URI); `media_type`+`data` carry base64 image
/// data (Anthropic). Providers use whichever is set.
struct ImageRef {
    std::string url;
    std::optional<std::string> media_type;
    std::optional<std::string> data;
};

/// Text content, optionally with attached images (multimodal). `images` empty
/// => plain text (the common case).
struct Content {
    std::string text;
    std::vector<ImageRef> images;
};

/// A function call the model wants to make.
struct ToolCall {
    std::string id;            // echoed back as tool_call_id
    std::string name;
    Json arguments = Json::object();
};

/// A single chat message in any role.
struct Message {
    Role role = Role::User;
    Content content;
    std::vector<ToolCall> tool_calls;      // assistant messages only
    std::optional<std::string> tool_call_id;  // role==Tool: which call this answers
    std::optional<std::string> name;          // role==Tool: tool name (some APIs)
};

/// Provider-agnostic tool spec: a JSON Schema for its parameters.
struct ToolDefinition {
    std::string name;
    std::string description;
    Json parameters = Json::object();
};

enum class FinishReason { Stop, Length, ToolCalls, ContentFilter, Error };

struct Usage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
};

struct GenerateOptions {
    std::optional<std::string> model;
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    std::optional<int> top_p;
    std::vector<std::string> stop;
    bool stream = false;
    std::vector<ToolDefinition> tools;
    /// OpenAI-style structured output: e.g. {"type":"json_object"} or a full
    /// json_schema. Serialized as-is by supporting providers.
    std::optional<Json> response_format;
    /// Tool selection: "auto" / "none" / "required", or {"type":"function",...}.
    std::optional<Json> tool_choice;
    /// Sampling seed (best-effort determinism), where supported.
    std::optional<long> seed;
};

// ---- Role string helpers --------------------------------------------------
const char* role_to_string(Role r) noexcept;
std::optional<Role> role_from_string(std::string_view s) noexcept;

// ---- nlohmann ADL serialization (canonical form) --------------------------
void to_json(Json& j, const Role& r);
void from_json(const Json& j, Role& r);
void to_json(Json& j, const Content& c);
void from_json(const Json& j, Content& c);
void to_json(Json& j, const ImageRef& i);
void from_json(const Json& j, ImageRef& i);
void to_json(Json& j, const ToolCall& t);
void from_json(const Json& j, ToolCall& t);
void to_json(Json& j, const Message& m);
void from_json(const Json& j, Message& m);
void to_json(Json& j, const ToolDefinition& t);
void from_json(const Json& j, ToolDefinition& t);
void to_json(Json& j, const Usage& u);
void from_json(const Json& j, Usage& u);
void to_json(Json& j, const GenerateOptions& g);
void from_json(const Json& j, GenerateOptions& g);

}  // namespace libagent
