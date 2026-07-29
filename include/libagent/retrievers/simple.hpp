#pragma once

#include "libagent/retriever.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace libagent {

/// A minimal in-memory retriever: ranks documents by distinct-token overlap
/// with the query (lowercased, alphanumeric tokenization). Deterministic and
/// dependency-free — good enough to wire RAG into an agent and to test the
/// integration. Swap for an embedding-backed retriever later without touching
/// the Agent.
class SimpleCorpusRetriever : public Retriever {
public:
    void add_document(std::string text) { docs_.push_back(std::move(text)); }
    [[nodiscard]] std::size_t size() const noexcept { return docs_.size(); }

    boost::asio::awaitable<std::vector<RetrievedChunk>>
    retrieve(const std::string& query, std::size_t k) override {
        std::vector<RetrievedChunk> results;
        if (docs_.empty() || k == 0) {
            co_return results;
        }
        const auto query_terms = tokenize(query);
        if (query_terms.empty()) {
            co_return results;
        }
        const std::unordered_set<std::string> query_set(query_terms.begin(),
                                                        query_terms.end());

        for (const auto& doc : docs_) {
            const auto doc_terms = tokenize(doc);
            const std::unordered_set<std::string> doc_set(doc_terms.begin(),
                                                          doc_terms.end());
            float score = 0.0f;
            for (const auto& term : query_set) {
                if (doc_set.count(term) != 0u) {
                    score += 1.0f;
                }
            }
            if (score > 0.0f) {
                results.push_back({doc, score});
            }
        }
        std::sort(results.begin(), results.end(),
                  [](const RetrievedChunk& a, const RetrievedChunk& b) {
                      return a.score > b.score;
                  });
        if (results.size() > k) {
            results.resize(k);
        }
        co_return results;
    }

private:
    static std::vector<std::string> tokenize(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) != 0) {
                cur.push_back(static_cast<char>(std::tolower(uc)));
            } else if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        }
        if (!cur.empty()) {
            out.push_back(std::move(cur));
        }
        return out;
    }

    std::vector<std::string> docs_;
};

}  // namespace libagent
