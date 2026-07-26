#pragma once

#include "libagent/agent.hpp"

#include <memory>
#include <string>
#include <vector>

namespace libagent {

/// A named, delegatable sub-agent.
struct AgentHandle {
    std::string name;
    std::shared_ptr<Agent> agent;
};

/// Coordinates multiple specialist agents by exposing each one as a tool to a
/// router agent. When the router's LLM decides to delegate, it calls the
/// synthesized tool, which runs the chosen sub-agent to completion and returns
/// its answer as the tool result. This reuses the Agent ReAct loop verbatim —
/// there is no separate runtime path.
class HandoffRouter {
public:
    /// `planner_provider` powers the router agent itself; each entry in
    /// `agents` becomes a delegatable tool.
    HandoffRouter(std::shared_ptr<LLMProvider> planner_provider,
                  std::shared_ptr<Memory> memory,
                  std::vector<AgentHandle> agents);

    /// Register an additional sub-agent at runtime.
    void add_agent(AgentHandle h);

    /// The underlying router agent (exposes co_run / run / co_run_stream).
    [[nodiscard]] Agent& agent();
    [[nodiscard]] std::shared_ptr<Agent> agent_ptr() const;

private:
    void register_as_tool(const AgentHandle& h);

    std::shared_ptr<Agent> router_;
    std::shared_ptr<ToolRegistry> tools_;
};

}  // namespace libagent
