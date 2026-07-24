// Real-endpoint smoke for the OpenAI-compatible provider.
// Build with -DLIBAGENT_BUILD_EXAMPLES=ON, then:
//   LIBAGENT_API_KEY=sk-... ./hello_agent
// Optional: LIBAGENT_BASE_URL, LIBAGENT_MODEL (e.g. for DeepSeek/Zhipu/Ollama).

#include "libagent/providers/openai.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

int main() {
    using namespace libagent;

    const char* key = std::getenv("LIBAGENT_API_KEY");
    if (key == nullptr || *key == '\0') {
        std::cout << "Set LIBAGENT_API_KEY (and optionally LIBAGENT_BASE_URL, "
                     "LIBAGENT_MODEL) to run this example.\n";
        return 0;
    }

    openai::Options opts;
    opts.api_key = key;
    if (const char* base = std::getenv("LIBAGENT_BASE_URL")) {
        opts.base_url = base;
    }
    std::string model = opts.default_model;
    if (const char* m = std::getenv("LIBAGENT_MODEL")) {
        model = m;
    }
    opts.default_model = model;

    openai::OpenAiProvider provider(std::move(opts));

    boost::asio::io_context ioc;
    auto fut = boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<ChatResponse> {
            ChatRequest req;
            req.options.model = model;
            req.messages.push_back(
                {Role::System, Content{"You are a concise assistant."}});
            req.messages.push_back({Role::User, Content{"Say hello in one sentence."}});
            co_return co_await provider.chat(req);
        },
        boost::asio::use_future);
    ioc.run();

    try {
        const ChatResponse resp = fut.get();
        std::cout << resp.message.content.text << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
