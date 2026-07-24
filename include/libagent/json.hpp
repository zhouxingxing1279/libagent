#pragma once

#include <nlohmann/json.hpp>

namespace libagent {

/// Single alias for JSON used across the public API. Backed by the vendored
/// nlohmann/json single header so downstream never needs find_package(json).
using Json = nlohmann::json;

}  // namespace libagent
