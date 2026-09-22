# libagent 面试学习笔记

> 目标：以 `stevevista/libagent` / 当前 fork 源码为基础，系统理解 C++20 Agent Runtime，并用于 C++ / AI Agent 相关面试准备。

## 学习主线

1. **Agent Runtime / ReAct Loop**
   - `include/libagent/agent.hpp`
   - `src/agent.cpp`
   - 重点：`run → co_run → step → prepare_request → provider → tool → memory → next round`

2. **核心数据模型**
   - 笔记：[02-types-and-message-model.md](./02-types-and-message-model.md)
   - `include/libagent/types.hpp`
   - `src/types.cpp`
   - `include/libagent/provider.hpp`
   - 重点：canonical data model、`Message`、`ToolCall`、`ToolDefinition`、`ChatRequest`、`ChatResponse`、Provider 协议适配

3. **Tool Calling**
   - 笔记：[03-tool-calling.md](./03-tool-calling.md)
   - `include/libagent/tool.hpp`
   - `src/tool.cpp`
   - `src/agent.cpp`
   - `examples/02_tool_agent.cpp`
   - 重点：`spec + handler`、Tool Registry、JSON Schema、LLM 工具选择、name -> handler 分发、Tool Result 回灌、并行 ToolCall

4. **C++20 Coroutine / Boost.Asio 异步模型**
   - `boost::asio::awaitable<T>`
   - `co_await` / `co_spawn`
   - 单线程 event loop 与 cooperative concurrency
   - 多 Tool fan-out / fan-in

5. **LLM Provider 抽象**
   - `include/libagent/provider.hpp`
   - `include/libagent/providers/openai.hpp`
   - `src/providers/`
   - 重点：Provider 与 Agent 解耦、OpenAI-compatible API、SSE streaming

6. **Memory / Context Management**
   - `include/libagent/memory.hpp`
   - `include/libagent/summarizing_memory.hpp`
   - 重点：FullMemory、WindowMemory、SummarizingMemory、compact()

7. **RAG**
   - `include/libagent/retriever.hpp`
   - `include/libagent/retrievers/`
   - 重点：retrieval 如何进入 prompt，而不是把 RAG 混入 Agent 控制逻辑

8. **Multi-Agent**
   - `include/libagent/orchestration.hpp`
   - `src/orchestration.cpp`
   - 核心思想：**Agent as Tool**

9. **工程化**
   - HTTP / TLS
   - Streaming
   - Cancellation
   - Logging / Hooks
   - Tests
   - CMake 与库导出

## 当前对项目的核心理解

libagent 是一个基于 **C++20 coroutine + Boost.Asio** 的异步 Agent Runtime。它将系统拆分为：

```text
User
  |
  v
Agent / ReAct Loop
  |
  +---- Memory
  +---- Retriever (RAG)
  +---- ToolRegistry
  |
  v
LLMProvider
  |
  v
OpenAI-compatible endpoint
```

Agent 的核心循环可以抽象为：

```cpp
while (round < max_tool_rounds) {
    request = co_await prepare_request();
    response = co_await provider->chat(request);

    memory->add(response.message);

    if (response.message.tool_calls.empty()) {
        return response.message.content.text;
    }

    co_await execute_tool_calls(response.message.tool_calls);
}
```

关键设计点：

- Agent 控制逻辑与具体模型协议通过 `LLMProvider` 解耦。
- Provider、Tool、Agent 都使用 `boost::asio::awaitable<T>`，统一异步执行模型。
- 同一轮多个 ToolCall 使用 `co_spawn + channel` 做并发 fan-out / fan-in。
- Tool 结果作为 Tool Message 写回 Memory，再触发下一轮模型推理。
- Multi-Agent 没有独立 runtime，而是将 sub-agent 包装为 Tool，即 **Agent-as-Tool**。
- Memory 与 Retriever 都是可替换抽象，不侵入 Agent 主循环。

## 面试准备原则

学习每个模块时都回答四个问题：

1. **它解决什么问题？**
2. **为什么这样设计？**
3. **源码具体如何实现？**
4. **如果让我重新设计，我会怎么改？**

后续每章都同时维护：

- 源码执行路径
- 关键 C++ 语法与机制
- 架构设计理由
- 常见面试追问
- 可直接复述的面试回答
- 可能存在的设计问题与改进方向

## 笔记目录

后续预计逐步增加：

```text
docs/interview-notes/
├── README.md
├── 01-agent-runtime.md
├── 02-types-and-message-model.md
├── 03-tool-calling.md
├── 04-coroutine-and-asio.md
├── 05-provider-and-http.md
├── 06-memory-and-context.md
├── 07-rag.md
├── 08-multi-agent.md
├── 09-engineering-design.md
└── 10-interview-questions.md
```

当前进度：

- 第一部分 Agent Runtime / ReAct Loop：已完成
- 第二部分核心数据模型：已完成
- 第三部分 Tool Calling：已完成基础链路

下一优先级：**继续深挖 ToolHandler 的 C++ 抽象与多 Tool 并发，再进入 C++20 Coroutine / Boost.Asio。**
