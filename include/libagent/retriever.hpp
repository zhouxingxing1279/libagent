#pragma once

#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <string>
#include <vector>

namespace libagent {

struct RetrievedChunk {
    std::string text;
    float score = 0.0f;
};

/// Optional RAG retriever (interface stub). Concrete backends (local corpus,
/// vector store, embedding provider) arrive in Phase 5.
class Retriever {
public:
    virtual ~Retriever() = default;
    virtual boost::asio::awaitable<std::vector<RetrievedChunk>>
    retrieve(const std::string& query, std::size_t k) = 0;
};

}  // namespace libagent
