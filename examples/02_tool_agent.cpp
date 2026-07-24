// Live agent demo: Agent + OpenAI-compatible provider + real tools.
// Demonstrates the full ReAct loop (model decides to call a tool -> the agent
// runs it -> result is fed back -> model produces the final answer).
//
// Build with -DLIBAGENT_BUILD_EXAMPLES=ON, then e.g.:
//   LIBAGENT_API_KEY=sk-... LIBAGENT_BASE_URL=https://api.deepseek.com \
//     LIBAGENT_MODEL=deepseek-chat ./tool_agent "What is the current UTC time?"
//
// Tool calls are traced to stderr so you can see the loop happen.

#include "libagent/agent.hpp"
#include "libagent/memory.hpp"
#include "libagent/providers/openai.hpp"
#include "libagent/tool.hpp"

#include <boost/asio/awaitable.hpp>

#include <ctime>
#include <exception>
#include <iostream>
#include <string>

namespace {

boost::asio::awaitable<libagent::Json> get_current_time(const libagent::Json& /*args*/) {
    std::cerr << "[tool] get_current_time() called\n";
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    const libagent::Json out = {{"iso", std::string(buf)},
                                {"epoch", static_cast<long long>(now)}};
    std::cerr << "[tool] -> " << out.dump() << "\n";
    co_return out;
}

boost::asio::awaitable<libagent::Json> calculate(const libagent::Json& args) {
    const double a = args.value("a", 0.0);
    const double b = args.value("b", 0.0);
    const std::string op = args.value("op", std::string{"+"});
    std::cerr << "[tool] calculate(a=" << a << ", op=" << op << ", b=" << b << ")\n";
    double r = 0.0;
    if (op == "+") {
        r = a + b;
    } else if (op == "-") {
        r = a - b;
    } else if (op == "*") {
        r = a * b;
    } else if (op == "/") {
        r = (b != 0.0 ? a / b : 0.0);
    }
    const libagent::Json out = {{"result", r}};
    std::cerr << "[tool] -> " << out.dump() << "\n";
    co_return out;
}

}  // namespace

int main(int argc, char** argv) {
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

    auto provider = std::make_shared<openai::OpenAiProvider>(std::move(opts));

    auto tools = std::make_shared<ToolRegistry>();
    tools->add({{"get_current_time",
                 "Get the current UTC time.",
                 Json{{"type", "object"}, {"properties", Json::object()}}},
                get_current_time});
    tools->add({{"calculate",
                 "Evaluate a basic arithmetic expression.",
                 Json{{"type", "object"},
                      {"properties",
                       Json{{"a", Json{{"type", "number"}}},
                            {"op", Json{{"type", "string"},
                                        {"enum", Json::array({"+", "-", "*", "/"})}}},
                            {"b", Json{{"type", "number"}}}}},
                      {"required", Json::array({"a", "op", "b"})}}},
                calculate});

    AgentOptions agent_opts;
    agent_opts.provider = provider;
    agent_opts.memory = std::make_shared<FullMemory>();
    agent_opts.tools = tools;
    agent_opts.system_prompt =
        "You are a helpful assistant. When a question can be answered with an "
        "available tool, call the tool instead of guessing or refusing.";

    const std::string prompt =
        (argc > 1) ? argv[1]
                   : "What is the current UTC time? Use the get_current_time tool.";

    std::cout << "user> " << prompt << "\n";
    Agent agent(std::move(agent_opts));
    try {
        const std::string answer = agent.run(prompt);
        std::cout << "agent> " << answer << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
