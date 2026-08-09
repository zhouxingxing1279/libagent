#pragma once

// Save/load a Memory (its message history) to/from JSON or a file. Uses the
// existing Message serialization, so it works for any Memory implementation
// (Full / Window / Summarizing). Does not change the Memory interface.

#include "libagent/json.hpp"
#include "libagent/memory.hpp"
#include "libagent/types.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace libagent {

/// Serialize a memory's history to a JSON array of messages.
inline Json serialize(const Memory& m) {
    Json j = Json::array();
    for (const auto& msg : m.history()) {
        j.push_back(msg);
    }
    return j;
}

/// Replace a memory's contents with the messages in `j` (a JSON array).
inline void load(Memory& m, const Json& j) {
    m.clear();
    if (!j.is_array()) {
        throw std::runtime_error("libagent: memory load expects a JSON array");
    }
    for (const auto& msg : j) {
        m.add(msg.get<Message>());
    }
}

/// Write a memory's history to a file as JSON.
inline void save_to_file(const Memory& m, const std::filesystem::path& path) {
    std::ofstream f(path);
    if (!f) {
        throw std::runtime_error("libagent: cannot open " + path.string() + " for writing");
    }
    f << serialize(m).dump();
}

/// Load a memory's history from a JSON file (replaces current contents).
inline void load_from_file(Memory& m, const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("libagent: cannot open " + path.string() + " for reading");
    }
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    load(m, Json::parse(content, nullptr, false));
}

}  // namespace libagent
