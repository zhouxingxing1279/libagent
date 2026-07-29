# libagent

A C++20 AI agent framework: LLM provider abstraction, a ReAct agent runtime with
tool calling, multi-agent orchestration, pluggable memory, and RAG retrieval —
all on coroutine-based async I/O.

> **Status:** core framework complete — 36/36 tests green, live-verified against
> OpenAI-compatible endpoints (DeepSeek). MIT licensed.

## Features

- **C++20 coroutines** (`boost::asio::awaitable<T>`) throughout — one executor
  model, no callbacks.
- **Provider-agnostic** `LLMProvider` interface; first impl is OpenAI-compatible
  (chat + SSE streaming), covering OpenAI, DeepSeek, Zhipu GLM, Moonshot, vLLM,
  Ollama via a configurable `base_url`.
- **ReAct agent** that calls tools, feeds results back, and loops until the final
  answer. Tool calls in a round fan out **concurrently**; the final answer can be
  **streamed live, token by token**.
- **Multi-agent orchestration** via `HandoffRouter` (each sub-agent is a tool the
  router delegates to).
- **Pluggable memory**: `FullMemory`, `WindowMemory`, `SummarizingMemory`
  (provider-summarized old turns); optional RAG via `SimpleCorpusRetriever`.
- **Cross-platform**: Linux / macOS / Windows; consumable downstream via
  `find_package(libagent)` with clean export hygiene.

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│ orchestration        HandoffRouter — agents as tools      │
├──────────────────────────────────────────────────────────┤
│ agent                ReAct loop • tools • memory • RAG    │
├──────────────────────────┬───────────────────────────────┤
│ memory                   │ retriever (RAG)                │
│ Full / Window / Summ.    │ SimpleCorpusRetriever / ...    │
├──────────────────────────┴───────────────────────────────┤
│ provider              LLMProvider (awaitable)             │
│   └ openai  (chat + SSE, configurable base_url)           │
├──────────────────────────────────────────────────────────┤
│ http                  coroutine HTTPS (Boost.Beast + TLS) │
├──────────────────────────────────────────────────────────┤
│ types • tool • streaming   (plain value types + sinks)    │
└──────────────────────────────────────────────────────────┘
```

## Quickstart

```cpp
#include <libagent/agent.hpp>
#include <libagent/memory.hpp>
#include <libagent/providers/openai.hpp>
#include <libagent/tool.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

using namespace libagent;
namespace asio = boost::asio;

asio::awaitable<Json> current_time(const Json&) {
    co_return Json{{"iso", "2026-07-29T00:00:00Z"}};
}

int main() {
    openai::Options opts;
    opts.api_key = "...";   // or set base_url for DeepSeek/Zhipu/Ollama/...
    auto provider = std::make_shared<openai::OpenAiProvider>(std::move(opts));

    auto tools = std::make_shared<ToolRegistry>();
    tools->add({{"current_time", "Get the current time",
                 Json{{"type", "object"}}},
                current_time});

    AgentOptions a;
    a.provider   = provider;
    a.memory     = std::make_shared<FullMemory>();
    a.tools      = tools;
    a.system_prompt = "Use tools when helpful.";
    Agent agent(a);

    asio::io_context ioc;
    std::string reply = asio::co_spawn(ioc, agent.co_run("What time is it?"),
                                       asio::use_future).get();
}
```

Live, runnable examples are in [`examples/`](examples/):

- `01_hello_agent` — basic chat against a real endpoint.
- `02_tool_agent` — full ReAct loop with tools (`get_current_time`, `calculate`).
- `03_handoff` — multi-agent router coordinating specialist sub-agents.

## Requirements

- C++20 compiler: GCC ≥ 11, Clang ≥ 14, Apple Clang ≥ 15, or MSVC ≥ 19.30
- CMake ≥ 3.22
- Boost (≥ 1.74; Asio/Beast are header-only for our usage) and OpenSSL

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options (all CMake): `BUILD_SHARED_LIBS` (off), `LIBAGENT_BUILD_TESTS` (on),
`LIBAGENT_BUILD_EXAMPLES` (off), `LIBAGENT_WITH_SSL` (on),
`LIBAGENT_WARNINGS_AS_ERRORS` (on), `LIBAGENT_BUILD_DOCS` (off).

### macOS

```bash
brew install boost openssl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DOpenSSL_ROOT=/opt/homebrew/opt/openssl
cmake --build build -j && ctest --test-dir build
```

### Windows (vcpkg)

```bash
vcpkg install boost openssl --triplet x64-windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release
```

### API documentation (Doxygen)

```bash
cmake -S . -B build -DLIBAGENT_BUILD_DOCS=ON
cmake --build build --target libagent_docs
# open build/docs/html/index.html
```

## Consume (after install)

```bash
cmake --install build --prefix /tmp/libagent-prefix
```

```cmake
# downstream project
find_package(libagent 0.1.0 REQUIRED)
target_link_libraries(my_app PRIVATE libagent::libagent)
```

The installed package is self-contained: vendored nlohmann/json ships in the
include tree, and only Boost + OpenSSL are re-resolved via the package config.

## Project layout

```
include/libagent/   public headers (types, provider, tool, memory, agent, …)
  providers/openai.hpp
  retrievers/simple.hpp
  summarizing_memory.hpp
src/                implementation (http client, openai provider, agent, …)
tests/              GoogleTest + CTest; fakes/ has FakeProvider + test HTTP server
examples/           runnable demos (gated on LIBAGENT_API_KEY)
third_party/        vendored nlohmann/json single header
```

## License

MIT — see [LICENSE](LICENSE).
