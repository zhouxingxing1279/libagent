#pragma once

#include "libagent/embedding.hpp"

#include <chrono>
#include <string>

namespace libagent::openai {

struct EmbedderOptions {
    std::string api_key;
    std::string base_url = "https://api.openai.com";
    std::string embeddings_path = "/v1/embeddings";
    std::string default_model = "text-embedding-3-small";

    int max_retries = 3;
    std::chrono::milliseconds initial_backoff{500};
    double backoff_multiplier = 2.0;
    std::chrono::milliseconds max_backoff{30000};
};

/// OpenAI-compatible embeddings provider. POSTs {model, input} and parses
/// `data[0].embedding`. Works with OpenAI, and OpenAI-compatible backends that
/// expose an embeddings endpoint.
class Embedder : public libagent::EmbeddingProvider {
public:
    explicit Embedder(EmbedderOptions opts);

    boost::asio::awaitable<std::vector<float>> embed(const std::string& text) override;

private:
    EmbedderOptions opts_;
    bool tls_ = true;
    std::string host_;
    std::string port_;
};

}  // namespace libagent::openai
