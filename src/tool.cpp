#include "libagent/tool.hpp"

#include <utility>

namespace libagent {

void ToolRegistry::add(Tool t) {
    auto name = t.spec.name;
    tools_.emplace(std::move(name), std::move(t));
}

bool ToolRegistry::remove(const std::string& name) {
    return tools_.erase(name) > 0;
}

const Tool* ToolRegistry::find(const std::string& name) const {
    const auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<ToolDefinition> ToolRegistry::schemas() const {
    std::vector<ToolDefinition> out;
    out.reserve(tools_.size());
    for (const auto& kv : tools_) {
        out.push_back(kv.second.spec);
    }
    return out;
}

Json ToolRegistry::to_provider_schema() const {
    Json arr = Json::array();
    for (const auto& kv : tools_) {
        const auto& spec = kv.second.spec;
        Json entry = Json::object();
        entry["name"] = spec.name;
        entry["description"] = spec.description;
        entry["parameters"] = spec.parameters;
        arr.push_back(std::move(entry));
    }
    return arr;
}

bool ToolRegistry::empty() const noexcept { return tools_.empty(); }

std::size_t ToolRegistry::size() const noexcept { return tools_.size(); }

}  // namespace libagent
