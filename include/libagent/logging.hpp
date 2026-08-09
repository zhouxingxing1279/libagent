#pragma once

#include <functional>
#include <string>

namespace libagent {

/// Severity for the simple text log sink.
enum class LogLevel { Debug, Info, Warn, Error };

/// A simple text log sink. Set `AgentOptions.log` to receive log lines from the
/// agent runtime (LLM calls, tool calls, errors). Keep it cheap and
/// non-blocking — it is invoked synchronously from the agent coroutine.
using LogSink = std::function<void(LogLevel, const std::string&)>;

}  // namespace libagent
