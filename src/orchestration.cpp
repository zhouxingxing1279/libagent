#include "libagent/orchestration.hpp"

#include "libagent/json.hpp"

#include <iostream>
#include <utility>

namespace libagent {

HandoffRouter::HandoffRouter(std::shared_ptr<LLMProvider> planner_provider,
                             std::shared_ptr<Memory> memory,
                             std::vector<AgentHandle> agents)
    : tools_(std::make_shared<ToolRegistry>()) {
    for (auto& a : agents) {
        register_as_tool(a);
    }

    AgentOptions opts;
    opts.provider = std::move(planner_provider);
    opts.memory = std::move(memory);
    opts.tools = tools_;
    opts.system_prompt =
        "You are a coordinating router. For each part of the user's request, "
        "delegate it to the most suitable available agent tool (passing a clear "
        "task description), then combine the returned results into a final "
        "answer. Do not attempt the work yourself if a suitable agent exists.";
    router_ = std::make_shared<Agent>(std::move(opts));
}

void HandoffRouter::register_as_tool(const AgentHandle& h) {
    Tool t;
    t.spec.name = h.name;
    t.spec.description = "Delegate a task to the '" + h.name + "' agent.";
    t.spec.parameters =
        Json{{"type", "object"},
             {"properties",
              Json{{"task", Json{{"type", "string"},
                                 {"description", "The task for this agent."}}}}},
             {"required", Json::array({"task"})}};

    std::shared_ptr<Agent> sub = h.agent;
    std::string name = h.name;
    t.handler = [sub, name](const Json& args) -> boost::asio::awaitable<Json> {
        const std::string task = args.value("task", std::string{});
        std::cerr << "[router] delegating to '" << name << "': " << task << "\n";
        std::string answer = co_await sub->co_run(task);
        std::cerr << "[router] '" << name << "' returned\n";
        co_return Json{{"result", std::move(answer)}};
    };

    tools_->add(std::move(t));
}

void HandoffRouter::add_agent(AgentHandle h) { register_as_tool(h); }

Agent& HandoffRouter::agent() { return *router_; }

std::shared_ptr<Agent> HandoffRouter::agent_ptr() const { return router_; }

}  // namespace libagent
