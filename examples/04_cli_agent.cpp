// Live end-to-end demo: an agent that can call real command-line tools via
// cli::command. The model decides which tool(s) to run; libagent exec's them,
// feeds the output back, and the model answers from it.
//
// Build with -DLIBAGENT_BUILD_EXAMPLES=ON, then e.g.:
//   LIBAGENT_API_KEY=sk-... LIBAGENT_BASE_URL=https://api.deepseek.com \
//     LIBAGENT_MODEL=deepseek-chat ./cli_agent \
//     "What is the current date and time, and who am I logged in as?"
//
// NOTE: the bundled tools use /bin/date, /bin/ls, /usr/bin/whoami (macOS/Linux).
// On Windows swap in the equivalent programs.

#include "libagent/agent.hpp"
#include "libagent/memory.hpp"
#include "libagent/providers/openai.hpp"
#include "libagent/tool.hpp"
#include "libagent/tools/cli.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace {

// Wrap a tool's handler to trace its calls to stderr.
libagent::Tool traced(libagent::Tool t) {
    const std::string name = t.spec.name;
    const auto inner = t.handler;
    t.handler = [name, inner](const libagent::Json& args) -> boost::asio::awaitable<libagent::Json> {
        std::cerr << "[tool] " << name << " args=" << args.dump() << "\n";
        const libagent::Json r = co_await inner(args);
        std::cerr << "[tool] " << name << " -> " << r.dump() << "\n";
        co_return r;
    };
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace libagent;
    namespace cli = libagent::cli;

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

    // Wrap a few real CLI programs as tools.
    auto tools = std::make_shared<ToolRegistry>();
    tools->add(traced(cli::command("current_time", "Get the current date and time.",
                                   {}, "/bin/date", {})));
    tools->add(traced(cli::command("whoami", "Get the current logged-in user.",
                                   {}, "/usr/bin/whoami", {})));
    tools->add(traced(cli::command("list_dir", "List entries in a directory.",
                                   {{"path", "string"}}, "/bin/ls", {"{path}"})));

    AgentOptions a;
    a.provider = provider;
    a.memory = std::make_shared<FullMemory>();
    a.tools = tools;
    a.system_prompt = "You are a helpful assistant. When a question can be "
                      "answered with an available tool, call the tool instead "
                      "of guessing.";

    const std::string prompt = (argc > 1)
        ? argv[1]
        : "What is the current date and time, and who am I logged in as?";

    std::cout << "user> " << prompt << "\n";
    Agent agent(std::move(a));
    try {
        const std::string reply = agent.run(prompt);
        std::cout << "agent> " << reply << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
