#pragma once

#include <boost/asio/awaitable.hpp>

#include <string>
#include <vector>

namespace libagent {

/// Produces dense float vectors for text, for use by vector-based retrievers.
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    /// Embed a single text into a vector.
    virtual boost::asio::awaitable<std::vector<float>> embed(const std::string& text) = 0;
};

}  // namespace libagent
