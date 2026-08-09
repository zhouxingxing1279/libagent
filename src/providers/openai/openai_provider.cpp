#include "libagent/providers/openai.hpp"

#include "http/https_client.hpp"
#include "http/retry.hpp"
#include "libagent/error.hpp"
#include "libagent/json.hpp"
#include "libagent/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace libagent::openai {

namespace {

namespace beasthttp = boost::beast::http;

FinishReason map_finish(const std::string& s) {
    if (s == "stop") return FinishReason::Stop;
    if (s == "length") return FinishReason::Length;
    if (s == "tool_calls") return FinishReason::ToolCalls;
    if (s == "content_filter") return FinishReason::ContentFilter;
    return FinishReason::Error;
}

ErrorCode classify_http_status(unsigned status) {
    if (status == 401 || status == 403) return ErrorCode::Auth;
    if (status == 429) return ErrorCode::RateLimited;
    if (status == 408) return ErrorCode::Timeout;
    return ErrorCode::Http;
}

Json build_body(const Options& o, const ChatRequest& req, bool stream) {
    Json j = Json::object();
    j["model"] = req.options.model.value_or(o.default_model);

    Json messages = Json::array();
    for (const auto& m : req.messages) {
        Json mj = Json::object();
        mj["role"] = role_to_string(m.role);
        if (m.role == Role::Assistant && !m.tool_calls.empty() && m.content.text.empty()) {
            mj["content"] = nullptr;
        } else {
            mj["content"] = m.content.text;
        }
        if (!m.tool_calls.empty()) {
            Json tcs = Json::array();
            for (const auto& tc : m.tool_calls) {
                tcs.push_back(Json{{"id", tc.id},
                                   {"type", "function"},
                                   {"function", Json{{"name", tc.name},
                                                     {"arguments", tc.arguments.dump()}}}});
            }
            mj["tool_calls"] = tcs;
        }
        if (m.tool_call_id) {
            mj["tool_call_id"] = *m.tool_call_id;
        }
        if (m.name) {
            mj["name"] = *m.name;
        }
        messages.push_back(std::move(mj));
    }
    j["messages"] = std::move(messages);

    if (req.options.temperature) j["temperature"] = *req.options.temperature;
    if (req.options.max_tokens) j["max_tokens"] = *req.options.max_tokens;
    if (req.options.top_p) j["top_p"] = *req.options.top_p;
    if (!req.options.stop.empty()) j["stop"] = req.options.stop;

    if (!req.options.tools.empty()) {
        Json tools = Json::array();
        for (const auto& t : req.options.tools) {
            // OpenAI/DeepSeek require function parameters to be a JSON Schema of
            // type "object". Coerce empty/missing-type schemas so callers can use
            // a bare {} for no-argument tools.
            Json params = t.parameters.is_object() ? t.parameters : Json::object();
            if (!params.contains("type")) {
                params["type"] = "object";
            }
            tools.push_back(Json{{"type", "function"},
                                 {"function", Json{{"name", t.name},
                                                   {"description", t.description},
                                                   {"parameters", std::move(params)}}}});
        }
        j["tools"] = std::move(tools);
    }

    if (req.options.response_format) j["response_format"] = *req.options.response_format;
    if (req.options.tool_choice) j["tool_choice"] = *req.options.tool_choice;
    if (req.options.seed) j["seed"] = *req.options.seed;

    if (stream) {
        j["stream"] = true;
    }
    return j;
}

ChatResponse parse_response(const Json& j) {
    ChatResponse out;
    const auto& choices = j.value("choices", Json::array());
    if (!choices.empty() && choices[0].contains("message")) {
        const auto& msg = choices[0]["message"];
        out.message.role = Role::Assistant;
        if (msg.contains("content") && !msg["content"].is_null()) {
            out.message.content.text = msg["content"].get<std::string>();
        }
        if (msg.contains("tool_calls")) {
            for (const auto& tc : msg["tool_calls"]) {
                ToolCall c;
                c.id = tc.value("id", "");
                if (tc.contains("function")) {
                    c.name = tc["function"].value("name", "");
                    const std::string args = tc["function"].value("arguments", "");
                    c.arguments = args.empty() ? Json::object()
                                                : Json::parse(args, nullptr, false);
                }
                out.message.tool_calls.push_back(std::move(c));
            }
        }
        out.finish = map_finish(choices[0].value("finish_reason", "stop"));
    }
    if (j.contains("usage") && j["usage"].is_object()) {
        out.usage = j["usage"].get<Usage>();
    }
    out.raw = j;
    return out;
}

http::Request make_http_request(const Options& o, bool tls, const std::string& host,
                                const std::string& port, const Json& body) {
    http::Request r;
    r.host = host;
    r.service = port;
    r.target = o.chat_path;
    r.method = beasthttp::verb::post;
    r.use_tls = tls;
    r.body = body.dump();
    r.headers.emplace_back("Authorization", "Bearer " + o.api_key);
    r.headers.emplace_back("Content-Type", "application/json");
    return r;
}

}  // namespace

OpenAiProvider::OpenAiProvider(Options opts) : opts_(std::move(opts)) {
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

boost::asio::awaitable<ChatResponse> OpenAiProvider::chat(const ChatRequest& req) {
    const Json body = build_body(opts_, req, false);
    http::Request hr = make_http_request(opts_, tls_, host_, port_, body);

    http::Response resp = co_await http::send_with_retry(
        hr, http::RetryPolicy{opts_.max_retries, opts_.initial_backoff,
                              opts_.backoff_multiplier, opts_.max_backoff});
    if (resp.status >= 400) {
        throw Error{classify_http_status(resp.status),
                    "OpenAI chat failed (HTTP " + std::to_string(resp.status) +
                        "): " + resp.body,
                    static_cast<int>(resp.status)};
    }

    const Json j = Json::parse(resp.body, nullptr, false);
    if (!j.is_object()) {
        throw Error{ErrorCode::Provider, "OpenAI chat: malformed JSON response"};
    }
    co_return parse_response(j);
}

boost::asio::awaitable<void> OpenAiProvider::stream(const ChatRequest& req,
                                                    TokenSink sink) {
    http::HttpsClient client;
    const Json body = build_body(opts_, req, true);
    http::Request hr = make_http_request(opts_, tls_, host_, port_, body);

    std::string buffer;
    FinishReason last_finish = FinishReason::Stop;
    std::optional<Usage> last_usage;
    std::vector<ToolCall> tool_calls;
    std::vector<std::string> tool_call_args;

    http::Response resp = co_await client.request_stream(
        hr, [&](std::string_view chunk) -> boost::asio::awaitable<void> {
            buffer.append(chunk);
            std::size_t pos;
            while ((pos = buffer.find("\n\n")) != std::string::npos) {
                std::string event = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);

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
                if (data.empty() || data == "[DONE]") {
                    continue;
                }
                const Json dj = Json::parse(data, nullptr, false);
                if (!dj.is_object()) {
                    continue;
                }
                const auto& choices = dj.value("choices", Json::array());
                if (!choices.empty()) {
                    const auto& ch = choices[0];
                    if (ch.contains("delta")) {
                        const auto& d = ch["delta"];
                        if (d.contains("content") && d["content"].is_string()) {
                            std::string text = d["content"].get<std::string>();
                            if (!text.empty()) {
                                StreamEvent de;
                                de.kind = StreamEvent::Kind::Delta;
                                de.delta = std::move(text);
                                co_await sink(de);
                            }
                        }
                        // Accumulate streamed tool calls by index (OpenAI sends
                        // id/name on the first chunk, arguments across many).
                        if (d.contains("tool_calls")) {
                            for (const auto& dtc : d["tool_calls"]) {
                                const int idx = dtc.value("index", 0);
                                if (idx < 0) {
                                    continue;
                                }
                                if (static_cast<std::size_t>(idx) >= tool_calls.size()) {
                                    tool_calls.resize(idx + 1);
                                    tool_call_args.resize(idx + 1);
                                }
                                if (dtc.contains("id")) {
                                    tool_calls[idx].id = dtc["id"].get<std::string>();
                                }
                                if (dtc.contains("function")) {
                                    const auto& fn = dtc["function"];
                                    if (fn.contains("name")) {
                                        tool_calls[idx].name += fn["name"].get<std::string>();
                                    }
                                    if (fn.contains("arguments")) {
                                        tool_call_args[idx] += fn["arguments"].get<std::string>();
                                    }
                                }
                            }
                        }
                    }
                    if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) {
                        last_finish = map_finish(ch["finish_reason"].get<std::string>());
                    }
                }
                if (dj.contains("usage") && dj["usage"].is_object()) {
                    last_usage = dj["usage"].get<Usage>();
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

    for (std::size_t i = 0; i < tool_calls.size(); ++i) {
        tool_calls[i].arguments =
            tool_call_args[i].empty()
                ? Json::object()
                : Json::parse(tool_call_args[i], nullptr, false);
    }

    StreamEvent fin;
    fin.kind = StreamEvent::Kind::Finish;
    fin.finish = last_finish;
    if (last_usage) {
        fin.usage = *last_usage;
    }
    fin.tool_calls = std::move(tool_calls);
    co_await sink(fin);
}

}  // namespace libagent::openai
