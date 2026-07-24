#pragma once

#include "libagent/json.hpp"
#include "libagent/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace libagent {

/// A tool handler is a coroutine returning the JSON result (or throwing).
/// Executed on the agent's executor.
using ToolHandler = std::function<boost::asio::awaitable<Json>(const Json&)>;

struct Tool {
    ToolDefinition spec;
    ToolHandler handler;
};

/// Registry of tools available to an agent. Tool definitions are
/// provider-agnostic JSON Schemas; each provider translates to its wire form.
class ToolRegistry {
public:
    void add(Tool t);
    bool remove(const std::string& name);

    [[nodiscard]] const Tool* find(const std::string& name) const;

    [[nodiscard]] std::vector<ToolDefinition> schemas() const;

    /// Provider-agnostic tools array: [{name, description, parameters}, ...].
    /// The OpenAI provider re-wraps each entry as {"type":"function","function":...}.
    [[nodiscard]] Json to_provider_schema() const;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, Tool> tools_;
};

}  // namespace libagent
