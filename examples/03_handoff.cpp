// Live multi-agent demo: a HandoffRouter coordinates two specialist agents
// (math + time). The router's LLM decides which agent(s) to delegate to; each
// sub-agent runs its own ReAct loop with its own tool.
//
// Build with -DLIBAGENT_BUILD_EXAMPLES=ON, then e.g.:
//   LIBAGENT_API_KEY=sk-... LIBAGENT_BASE_URL=https://api.deepseek.com \
//     LIBAGENT_MODEL=deepseek-chat ./handoff_agent \
//     "What is 15 times 12, and what is the current UTC time?"

#include "libagent/agent.hpp"
#include "libagent/memory.hpp"
#include "libagent/orchestration.hpp"
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
    co_return libagent::Json{{"iso", std::string(buf)},
                             {"epoch", static_cast<long long>(now)}};
}

boost::asio::awaitable<libagent::Json> calculate(const libagent::Json& args) {
    const double a = args.value("a", 0.0);
    const double b = args.value("b", 0.0);
    const std::string op = args.value("op", std::string{"+"});
    std::cerr << "[tool] calculate(a=" << a << ", op=" << op << ", b=" << b << ")\n";
    double r = 0.0;
    if (op == "+") r = a + b;
    else if (op == "-") r = a - b;
    else if (op == "*") r = a * b;
    else if (op == "/") r = (b != 0.0 ? a / b : 0.0);
    co_return libagent::Json{{"result", r}};
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

    auto make_provider = [&] {
        openai::Options o;
        o.api_key = key;
        if (const char* base = std::getenv("LIBAGENT_BASE_URL")) o.base_url = base;
        std::string model = o.default_model;
        if (const char* m = std::getenv("LIBAGENT_MODEL")) model = m;
        o.default_model = model;
        return std::make_shared<openai::OpenAiProvider>(std::move(o));
    };

    // Math specialist.
    auto math_tools = std::make_shared<ToolRegistry>();
    math_tools->add({{"calculate",
                      "Evaluate a basic arithmetic expression.",
                      Json{{"type", "object"},
                           {"properties",
                            Json{{"a", Json{{"type", "number"}}},
                                 {"op", Json{{"type", "string"},
                                             {"enum", Json::array({"+", "-", "*", "/"})}}},
                                 {"b", Json{{"type", "number"}}}}},
                           {"required", Json::array({"a", "op", "b"})}}},
                     calculate});
    AgentOptions math_opts;
    math_opts.provider = make_provider();
    math_opts.memory = std::make_shared<FullMemory>();
    math_opts.tools = math_tools;
    math_opts.system_prompt = "You are a math specialist. Use the calculate tool.";
    auto math_agent = std::make_shared<Agent>(math_opts);

    // Time specialist.
    auto time_tools = std::make_shared<ToolRegistry>();
    time_tools->add({{"get_current_time",
                      "Get the current UTC time.",
                      Json{{"type", "object"}, {"properties", Json::object()}}},
                     get_current_time});
    AgentOptions time_opts;
    time_opts.provider = make_provider();
    time_opts.memory = std::make_shared<FullMemory>();
    time_opts.tools = time_tools;
    time_opts.system_prompt = "You are a time specialist. Use get_current_time.";
    auto time_agent = std::make_shared<Agent>(time_opts);

    // Router coordinates the two specialists.
    HandoffRouter router(make_provider(), std::make_shared<FullMemory>(),
                         {AgentHandle{"math_agent", math_agent},
                          AgentHandle{"time_agent", time_agent}});

    const std::string prompt = (argc > 1)
        ? argv[1]
        : "What is 15 times 12, and what is the current UTC time?";

    std::cout << "user> " << prompt << "\n";
    try {
        const std::string answer = router.agent().run(prompt);
        std::cout << "router> " << answer << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
