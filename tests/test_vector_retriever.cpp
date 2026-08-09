#include "libagent/retrievers/vector.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace libagent;
using boost::asio::awaitable;

// Deterministic bag-of-words embedder over a tiny fixed vocab — enough to give
// VectorRetriever meaningful (non-random) vectors for offline tests.
class BagEmbedder : public EmbeddingProvider {
public:
    std::vector<std::string> vocab{"cat",  "dog",  "sat",   "ran", "paris",
                                   "capital", "france", "the", "of"};

    boost::asio::awaitable<std::vector<float>> embed(const std::string& text) override {
        std::vector<float> v(vocab.size(), 0.0F);
        std::string word;
        auto flush = [&] {
            if (word.empty()) {
                return;
            }
            for (std::size_t i = 0; i < vocab.size(); ++i) {
                if (vocab[i] == word) {
                    v[i] += 1.0F;
                }
            }
            word.clear();
        };
        for (char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            } else {
                flush();
            }
        }
        flush();
        co_return v;
    }
};

TEST(VectorRetriever, RanksByCosineSimilarity) {
    auto embedder = std::make_shared<BagEmbedder>();
    auto retriever = std::make_shared<VectorRetriever>(embedder);

    boost::asio::io_context ioc;
    std::vector<RetrievedChunk> res;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> awaitable<void> {
            co_await retriever->add("the cat sat on the mat");
            co_await retriever->add("the dog ran fast");
            co_await retriever->add("paris is the capital of france");
            res = co_await retriever->retrieve("cat sat", 2);
            co_return;
        },
        boost::asio::use_future);
    ioc.run();
    fut.get();

    ASSERT_EQ(retriever->size(), 3u);
    ASSERT_FALSE(res.empty());
    EXPECT_EQ(res[0].text, "the cat sat on the mat");  // shares cat+sat
    EXPECT_GT(res[0].score, 0.0F);
}

TEST(VectorRetriever, ChunkTextSplitsOnBoundaries) {
    const auto chunks = chunk_text("aaabbbcccdddeee", 4);
    ASSERT_FALSE(chunks.empty());
    for (const auto& c : chunks) {
        EXPECT_LE(c.size(), 4u);
    }
}

}  // namespace
