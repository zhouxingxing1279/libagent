#pragma once

#include "libagent/embedding.hpp"
#include "libagent/retriever.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace libagent {

/// A retriever that ranks documents by cosine similarity between the query's
/// embedding and each document's embedding (computed once at add time). Swap in
/// any EmbeddingProvider (OpenAI-compatible, local model, ...).
class VectorRetriever : public Retriever {
public:
    explicit VectorRetriever(std::shared_ptr<EmbeddingProvider> embedder)
        : embedder_(std::move(embedder)) {}

    /// Embed and store a document. Async because embedding is.
    boost::asio::awaitable<void> add(std::string text) {
        auto vec = co_await embedder_->embed(text);
        docs_.emplace_back(std::move(text), std::move(vec));
        co_return;
    }

    [[nodiscard]] std::size_t size() const noexcept { return docs_.size(); }

    boost::asio::awaitable<std::vector<RetrievedChunk>>
    retrieve(const std::string& query, std::size_t k) override {
        std::vector<RetrievedChunk> results;
        if (docs_.empty() || k == 0) {
            co_return results;
        }
        const auto q = co_await embedder_->embed(query);
        std::vector<std::pair<float, std::size_t>> scored;
        scored.reserve(docs_.size());
        for (std::size_t i = 0; i < docs_.size(); ++i) {
            scored.emplace_back(cosine(q, docs_[i].second), i);
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (std::size_t r = 0; r < scored.size() && r < k; ++r) {
            if (scored[r].first <= 0.0F) {
                break;
            }
            results.push_back({docs_[scored[r].second].first, scored[r].first});
        }
        co_return results;
    }

private:
    static float cosine(const std::vector<float>& a, const std::vector<float>& b) {
        const std::size_t n = std::min(a.size(), b.size());
        float dot = 0.0F;
        float na = 0.0F;
        float nb = 0.0F;
        for (std::size_t i = 0; i < n; ++i) {
            dot += a[i] * b[i];
            na += a[i] * a[i];
            nb += b[i] * b[i];
        }
        if (na == 0.0F || nb == 0.0F) {
            return 0.0F;
        }
        return dot / (std::sqrt(na) * std::sqrt(nb));
    }

    std::shared_ptr<EmbeddingProvider> embedder_;
    std::vector<std::pair<std::string, std::vector<float>>> docs_;
};

/// Split `text` into chunks of at most `max_chars` on whitespace boundaries.
inline std::vector<std::string> chunk_text(const std::string& text, std::size_t max_chars) {
    std::vector<std::string> chunks;
    std::string cur;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t end = std::min(i + max_chars, text.size());
        cur = text.substr(i, end - i);
        if (!chunks.empty() && chunks.back().size() + cur.size() + 1 <= max_chars) {
            chunks.back() += ' ';
            chunks.back() += cur;
        } else {
            chunks.push_back(std::move(cur));
        }
        i = end;
    }
    return chunks;
}

}  // namespace libagent
