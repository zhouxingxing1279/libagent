# libagent

A C++20 AI agent framework: LLM provider abstraction, single-agent tool-calling
loop (ReAct), multi-agent orchestration, and pluggable memory / retrieval.

> **Status:** Phase 0 — build system scaffold + toolchain validation. The agent
> runtime, providers, and tools arrive in later phases (see the project plan).

## Features (target)

- C++20 coroutine-based async I/O (`boost::asio::awaitable<T>`)
- Provider-agnostic `LLMProvider` interface (first impl: OpenAI-compatible)
- Tool / function-calling with a ReAct agent loop
- In-memory and pluggable memory; optional RAG retriever
- Cross-platform: Linux / macOS / Windows
- CMake `install`/`export` — consumable via `find_package(libagent)`

## Requirements

- C++20 compiler: GCC ≥ 11, Clang ≥ 14, Apple Clang ≥ 15, or MSVC ≥ 19.30
- CMake ≥ 3.22
- Boost (system/asio/beast) and OpenSSL, provided by the system/vcpkg/Homebrew

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### macOS

```bash
brew install boost openssl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DOpenSSL_ROOT=/opt/homebrew/opt/openssl
cmake --build build -j && ctest --test-dir build
```

### Windows (vcpkg)

```bash
vcpkg install boost-system openssl --triplet x64-windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release
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

## License

MIT — see [LICENSE](LICENSE).
