#include "libagent/providers/anthropic.hpp"

#include "http/https_client.hpp"
#include "http/retry.hpp"
#include "libagent/error.hpp"
#include "libagent/json.hpp"
#include "libagent/types.hpp"

#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libagent::anthropic {

namespace {

namespace beasthttp = boost::beast::http;

FinishReason map_stop(const std::string& s) {
    if (s == "end_turn" || s == "stop_sequence") return FinishReason::Stop;
    if (s == "tool_use") return FinishReason::ToolCalls;
    if (s == "max_tokens") return FinishReason::Length;
    return FinishReason::Error;
}

ErrorCode classify_http_status(unsigned status) {
    if (status == 401 || status == 403) return ErrorCode::Auth;
    if (status == 429) return ErrorCode::RateLimited;
    if (status == 408) return ErrorCode::Timeout;
    return ErrorCode::Http;
}

// Build the Anthropic "messages" array from libagent messages. System messages
// are handled separately (top-level field); consecutive tool results are merged
// into a single user turn (Anthropic requires alternating user/assistant turns).
Json build_messages(const std::vector<Message>& msgs) {
    Json out = Json::array();
    for (std::size_t i = 0; i < msgs.size();) {
        const Message& m = msgs[i];
        if (m.role == Role::Tool) {
            Json content = Json::array();
            while (i < msgs.size() && msgs[i].role == Role::Tool) {
                content.push_back(Json{{"type", "tool_result"},
                                       {"tool_use_id", msgs[i].tool_call_id.value_or("")},
                                       {"content", msgs[i].content.text}});
                ++i;
            }
            out.push_back(Json{{"role", "user"}, {"content", std::move(content)}});
        } else if (m.role == Role::Assistant) {
            Json content = Json::array();
            if (!m.content.text.empty()) {
                content.push_back(Json{{"type", "text"}, {"text", m.content.text}});
            }
            for (const auto& tc : m.tool_calls) {
                content.push_back(Json{{"type", "tool_use"},
                                       {"id", tc.id},
                                       {"name", tc.name},
                                       {"input", tc.arguments}});
            }
            if (content.empty()) {
                content.push_back(Json{{"type", "text"}, {"text", ""}});
            }
            out.push_back(Json{{"role", "assistant"}, {"content", std::move(content)}});
            ++i;
        } else {  // Role::User
            out.push_back(Json{{"role", "user"}, {"content", m.content.text}});
            ++i;
        }
    }
    return out;
}

Json build_body(const Options& o, const ChatRequest& req, bool stream) {
    Json j = Json::object();
    j["model"] = req.options.model.value_or(o.default_model);
    j["max_tokens"] = req.options.max_tokens.value_or(1024);  // required by Anthropic

    std::string system_text;
    std::vector<Message> non_system;
    for (const auto& m : req.messages) {
        if (m.role == Role::System) {
            if (!system_text.empty()) {
                system_text += '\n';
            }
            system_text += m.content.text;
        } else {
            non_system.push_back(m);
        }
    }
    if (!system_text.empty()) {
        j["system"] = std::move(system_text);
    }
    j["messages"] = build_messages(non_system);

    if (req.options.temperature) j["temperature"] = *req.options.temperature;
    if (req.options.top_p) j["top_p"] = *req.options.top_p;
    if (!req.options.stop.empty()) j["stop_sequences"] = req.options.stop;

    if (!req.options.tools.empty()) {
        Json tools = Json::array();
        for (const auto& t : req.options.tools) {
            Json schema = t.parameters.is_object() ? t.parameters : Json::object();
            if (!schema.contains("type")) {
                schema["type"] = "object";
            }
            tools.push_back(Json{{"name", t.name},
                                 {"description", t.description},
                                 {"input_schema", std::move(schema)}});
        }
        j["tools"] = std::move(tools);
    }

    if (stream) {
        j["stream"] = true;
    }
    return j;
}

ChatResponse parse_response(const Json& j) {
    ChatResponse out;
    out.message.role = Role::Assistant;
    if (j.contains("content") && j["content"].is_array()) {
        for (const auto& block : j["content"]) {
            const std::string type = block.value("type", "");
            if (type == "text") {
                if (!out.message.content.text.empty()) {
                    out.message.content.text += '\n';
                }
                out.message.content.text += block.value("text", std::string{});
            } else if (type == "tool_use") {
                ToolCall tc;
                tc.id = block.value("id", "");
                tc.name = block.value("name", "");
                tc.arguments = block.value("input", Json::object());
                out.message.tool_calls.push_back(std::move(tc));
            }
        }
    }
    out.finish = map_stop(j.value("stop_reason", "end_turn"));
    if (j.contains("usage") && j["usage"].is_object()) {
        const auto& u = j["usage"];
        out.usage.prompt_tokens = u.value("input_tokens", 0);
        out.usage.completion_tokens = u.value("output_tokens", 0);
        out.usage.total_tokens = out.usage.prompt_tokens + out.usage.completion_tokens;
    }
    out.raw = j;
    return out;
}

http::Request make_http_request(const Options& o, bool tls, const std::string& host,
                                const std::string& port, const Json& body) {
    http::Request r;
    r.host = host;
    r.service = port;
    r.target = o.messages_path;
    r.method = beasthttp::verb::post;
    r.use_tls = tls;
    r.body = body.dump();
    r.headers.emplace_back("x-api-key", o.api_key);
    r.headers.emplace_back("anthropic-version", o.anthropic_version);
    r.headers.emplace_back("Content-Type", "application/json");
    return r;
}

http::RetryPolicy retry_policy(const Options& o) {
    return http::RetryPolicy{o.max_retries, o.initial_backoff, o.backoff_multiplier,
                             o.max_backoff};
}

// Extract the `data:` payload from one SSE event (between blank-line boundaries).
std::string event_data(const std::string& event) {
    std::string data;
    std::istringstream iss(event);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data:", 0) == 0) {
            std::string p = line.substr(5);
            if (!p.empty() && p[0] == ' ') {
                p.erase(0, 1);
            }
            data += p;
        }
    }
    return data;
}

}  // namespace

AnthropicProvider::AnthropicProvider(Options opts) : opts_(std::move(opts)) {
    const std::string& url = opts_.base_url;
    bool tls = true;
    std::string rest = url;
    if (url.rfind("https://", 0) == 0) {
        tls = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        tls = false;
        rest = url.substr(7);
    }
    if (const auto slash = rest.find('/'); slash != std::string::npos) {
        rest = rest.substr(0, slash);
    }
    std::string host = rest;
    std::string port = tls ? "443" : "80";
    if (const auto colon = rest.find(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
    }
    tls_ = tls;
    host_ = std::move(host);
    port_ = std::move(port);
}

boost::asio::awaitable<ChatResponse> AnthropicProvider::chat(const ChatRequest& req) {
    const Json body = build_body(opts_, req, false);
    http::Request hr = make_http_request(opts_, tls_, host_, port_, body);

    http::Response resp = co_await http::send_with_retry(hr, retry_policy(opts_));
    if (resp.status >= 400) {
        throw Error{classify_http_status(resp.status),
                    "Anthropic chat failed (HTTP " + std::to_string(resp.status) +
                        "): " + resp.body,
                    static_cast<int>(resp.status)};
    }
    const Json j = Json::parse(resp.body, nullptr, false);
    if (!j.is_object()) {
        throw Error{ErrorCode::Provider, "Anthropic chat: malformed JSON response"};
    }
    co_return parse_response(j);
}

boost::asio::awaitable<void> AnthropicProvider::stream(const ChatRequest& req,
                                                       TokenSink sink) {
    const Json body = build_body(opts_, req, true);
    http::Request hr = make_http_request(opts_, tls_, host_, port_, body);

    std::string buffer;
    FinishReason finish = FinishReason::Stop;
    Usage usage;

    struct ToolAcc {
        bool is_tool = false;
        std::string id;
        std::string name;
        std::string args;
    };
    std::vector<ToolAcc> tools;

    http::HttpsClient client;
    http::Response resp = co_await client.request_stream(
        hr, [&](std::string_view chunk) -> boost::asio::awaitable<void> {
            buffer.append(chunk);
            std::size_t pos;
            while ((pos = buffer.find("\n\n")) != std::string::npos) {
                std::string event = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);

                const std::string data = event_data(event);
                if (data.empty()) {
                    continue;
                }
                const Json dj = Json::parse(data, nullptr, false);
                if (!dj.is_object()) {
                    continue;
                }
                const std::string type = dj.value("type", "");

                if (type == "content_block_start" && dj.contains("content_block")) {
                    const auto& cb = dj["content_block"];
                    const int idx = dj.value("index", 0);
                    if (static_cast<std::size_t>(idx) >= tools.size()) {
                        tools.resize(idx + 1);
                    }
                    if (cb.value("type", "") == "tool_use") {
                        tools[idx].is_tool = true;
                        tools[idx].id = cb.value("id", "");
                        tools[idx].name = cb.value("name", "");
                    }
                } else if (type == "content_block_delta" && dj.contains("delta")) {
                    const int idx = dj.value("index", 0);
                    const auto& d = dj["delta"];
                    const std::string dt = d.value("type", "");
                    if (dt == "text_delta") {
                        std::string text = d.value("text", std::string{});
                        if (!text.empty()) {
                            StreamEvent de;
                            de.kind = StreamEvent::Kind::Delta;
                            de.delta = std::move(text);
                            co_await sink(de);
                        }
                    } else if (dt == "input_json_delta") {
                        if (static_cast<std::size_t>(idx) < tools.size()) {
                            tools[idx].args += d.value("partial_json", std::string{});
                        }
                    }
                } else if (type == "message_delta") {
                    if (dj.contains("delta") && dj["delta"].contains("stop_reason") &&
                        dj["delta"]["stop_reason"].is_string()) {
                        finish = map_stop(dj["delta"]["stop_reason"].get<std::string>());
                    }
                    if (dj.contains("usage") && dj["usage"].is_object()) {
                        usage.completion_tokens = dj["usage"].value("output_tokens", 0);
                    }
                } else if (type == "message_start" && dj.contains("message") &&
                           dj["message"].contains("usage")) {
                    const auto& u = dj["message"]["usage"];
                    usage.prompt_tokens = u.value("input_tokens", 0);
                }
            }
            co_return;
        });

    if (resp.status >= 400) {
        StreamEvent err;
        err.kind = StreamEvent::Kind::Error;
        err.error = "HTTP " + std::to_string(resp.status);
        co_await sink(err);
        co_return;
    }

    std::vector<ToolCall> tool_calls;
    for (auto& t : tools) {
        if (t.is_tool) {
            ToolCall tc;
            tc.id = t.id;
            tc.name = t.name;
            tc.arguments =
                t.args.empty() ? Json::object() : Json::parse(t.args, nullptr, false);
            tool_calls.push_back(std::move(tc));
        }
    }

    StreamEvent fin;
    fin.kind = StreamEvent::Kind::Finish;
    fin.finish = finish;
    fin.usage = usage;
    fin.tool_calls = std::move(tool_calls);
    co_await sink(fin);
}

}  // namespace libagent::anthropic
