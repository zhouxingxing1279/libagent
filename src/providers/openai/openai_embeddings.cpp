#include "libagent/providers/openai_embeddings.hpp"

#include "http/https_client.hpp"
#include "http/retry.hpp"
#include "libagent/error.hpp"
#include "libagent/json.hpp"

#include <boost/beast/http.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace libagent::openai {

namespace {
namespace beasthttp = boost::beast::http;
}

Embedder::Embedder(EmbedderOptions opts) : opts_(std::move(opts)) {
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

boost::asio::awaitable<std::vector<float>> Embedder::embed(const std::string& text) {
    const Json body = Json{{"model", opts_.default_model}, {"input", text}};

    http::Request r;
    r.host = host_;
    r.service = port_;
    r.target = opts_.embeddings_path;
    r.method = beasthttp::verb::post;
    r.use_tls = tls_;
    r.body = body.dump();
    r.headers.emplace_back("Authorization", "Bearer " + opts_.api_key);
    r.headers.emplace_back("Content-Type", "application/json");

    http::Response resp = co_await http::send_with_retry(
        r, http::RetryPolicy{opts_.max_retries, opts_.initial_backoff,
                             opts_.backoff_multiplier, opts_.max_backoff});
    if (resp.status >= 400) {
        throw Error{ErrorCode::Http,
                    "OpenAI embeddings failed (HTTP " + std::to_string(resp.status) + "): " +
                        resp.body,
                    static_cast<int>(resp.status)};
    }

    const Json j = Json::parse(resp.body, nullptr, false);
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array() ||
        j["data"].empty() || !j["data"][0].contains("embedding")) {
        throw Error{ErrorCode::Provider, "OpenAI embeddings: malformed response"};
    }

    std::vector<float> vec;
    const Json& emb = j["data"][0]["embedding"];
    vec.reserve(emb.size());
    for (const auto& v : emb) {
        vec.push_back(v.get<float>());
    }
    co_return vec;
}

}  // namespace libagent::openai
