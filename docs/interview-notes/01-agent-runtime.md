# 01. Agent Runtime / ReAct Loop

## 1. 这一章要解决什么问题

这一章只回答一个核心问题：

> **当用户调用 `agent.run("...")` 后，libagent 内部到底发生了什么？**

## 1.1 本章源码定位

| 主题 | 代码位置 | 关键符号 |
| --- | --- | --- |
| Agent 配置与接口 | `include/libagent/agent.hpp` | `AgentOptions`, `Agent` |
| Agent Runtime 主实现 | `src/agent.cpp` | `Agent::run`, `Agent::co_run`, `Agent::step`, `Agent::prepare_request` |
| 消息写入 | `src/agent.cpp` | `Agent::remember` |
| Tool 执行 | `src/agent.cpp` | `Agent::execute_tool`, `Agent::execute_tool_calls` |
| Message / ToolCall 数据结构 | `include/libagent/types.hpp` | `Message`, `ToolCall`, `Role`, `GenerateOptions` |
| Tool 注册表 | `include/libagent/tool.hpp` | `Tool`, `ToolHandler`, `ToolRegistry` |
| Memory 抽象 | `include/libagent/memory.hpp` | `Memory`, `FullMemory`, `WindowMemory` |

后文每个源码分析小节都以“**代码位置**”开头，便于直接跳转源码核对。

理解目标是能够完整描述：

```text
run
  -> co_run
  -> step
  -> prepare_request
  -> provider->chat
  -> remember assistant message
  -> execute_tool_calls
  -> remember tool result
  -> next step
  -> final answer
```

---

## 2. Agent 的职责边界

`Agent` 不是模型，也不是工具，也不是记忆系统。

它更接近一个 **Agent Runtime / Orchestrator**。

它负责：

- 保存用户消息
- 请求 Memory 做必要压缩
- 调用 Retriever 获取 RAG context
- 从 ToolRegistry 获取工具 schema
- 组装 ChatRequest
- 调用 LLMProvider
- 保存 Assistant Message
- 检查 ToolCall
- 执行 Tool
- 保存 Tool Result
- 决定是否继续下一轮
- 达到最大轮数时终止

因此可以把它看成一个状态机：

```text
USER_INPUT
   |
   v
PREPARE_REQUEST
   |
   v
CALL_LLM
   |
   +---- no tool_calls ----> FINAL
   |
   v
EXECUTE_TOOLS
   |
   v
STORE_RESULTS
   |
   +-----------------------> PREPARE_REQUEST
```

---

## 3. AgentOptions：Agent 的依赖注入入口

**代码位置：** `include/libagent/agent.hpp` → `struct AgentOptions`；`src/agent.cpp` → `Agent::Agent`


`Agent` 自己不创建 Provider、Memory、Retriever 和 ToolRegistry。

构造时：

```cpp
Agent::Agent(AgentOptions opts)
    : opts_(std::move(opts)) {}
```

主要依赖：

```cpp
std::shared_ptr<LLMProvider> provider;
std::shared_ptr<Memory> memory;
std::shared_ptr<ToolRegistry> tools;
std::shared_ptr<Retriever> retriever;
```

这属于典型的依赖注入。

意义是 Agent Runtime 只负责调度，不绑定具体实现。

例如：

```text
                Agent
                 |
     +-----------+-----------+
     |           |           |
 LLMProvider   Memory   ToolRegistry
     ^           ^           ^
     |           |           |
  OpenAI     FullMemory   custom tools
  Ollama     WindowMemory
  ...
```

---

## 4. 外部入口：run()

**代码位置：** `src/agent.cpp` → `Agent::run(std::string user_input)`；取消版本同文件中的 `Agent::run(..., cancellation_slot)`


同步接口：

```cpp
std::string Agent::run(std::string user_input) {
    boost::asio::io_context ioc;

    auto fut = boost::asio::co_spawn(
        ioc,
        co_run(std::move(user_input)),
        boost::asio::use_future
    );

    ioc.run();
    return fut.get();
}
```

这里完成了三件事：

1. 创建 `io_context`
2. 将协程版本 `co_run()` 放到 executor 上执行
3. 当前线程运行事件循环，并最终从 future 获取结果

因此：

> `run()` 是同步包装层，真正的 Agent 逻辑在 `co_run()`。

调用关系：

```text
caller
  |
  v
Agent::run()
  |
  +-- create io_context
  |
  +-- co_spawn(co_run())
  |
  +-- ioc.run()
  |
  +-- future.get()
  |
  v
std::string answer
```

---

## 5. co_run()：真正的 ReAct 主循环

**代码位置：** `src/agent.cpp` → `Agent::co_run`


源码核心：

```cpp
boost::asio::awaitable<std::string>
Agent::co_run(std::string user_input) {

    remember({
        Role::User,
        Content{std::move(user_input)}
    });

    for (int step_n = 0;
         step_n < opts_.max_tool_rounds;
         ++step_n) {

        const ChatResponse resp = co_await step();

        if (resp.message.tool_calls.empty()) {
            co_return resp.message.content.text;
        }
    }

    co_return "[libagent] max_tool_rounds reached";
}
```

这个函数实际上非常简单。

可以压缩为：

```cpp
save_user_message();

while (not_over_limit) {
    response = co_await one_step();

    if (no_tools_requested)
        return final_answer;
}
```

关键退出条件不是 `FinishReason::Stop`，而是：

```cpp
resp.message.tool_calls.empty()
```

也就是说：

> **只要 Assistant Message 里仍有 tool_calls，Agent 就认为任务还没结束。**

---

## 6. remember()：统一的消息写入入口

**代码位置：** `src/agent.cpp` → `Agent::remember`；消息结构定义见 `include/libagent/types.hpp` → `Message`


```cpp
void Agent::remember(Message m) {
    if (opts_.hooks.on_message) {
        opts_.hooks.on_message(m);
    }

    opts_.memory->add(std::move(m));
}
```

它做两件事：

1. 发出 observability hook
2. 将 Message 写入 Memory

Agent 内部所有重要消息都通过该入口写入：

- User Message
- Assistant Message
- Tool Result Message

因此一轮工具调用后，历史大致是：

```text
User:
  深圳今天天气如何？

Assistant:
  tool_calls = [
      weather(...)
  ]

Tool:
  {"temperature": 30, ...}

Assistant:
  深圳今天...
```

注意：

> Tool result 不是直接作为 C++ 返回值传给下一轮 LLM，而是先变成 `Message{Role::Tool}` 写入 Memory，下一轮重新组装到 ChatRequest。

这是 Agent tool calling 的关键机制。

---

## 7. step()：一轮 Agent 推理

**代码位置：** `src/agent.cpp` → `Agent::step`


```cpp
boost::asio::awaitable<ChatResponse>
Agent::step() {

    ChatRequest req = co_await prepare_request();

    ChatResponse resp =
        co_await opts_.provider->chat(req);

    remember(resp.message);

    co_await execute_tool_calls(
        resp.message.tool_calls
    );

    co_return resp;
}
```

一轮 step 可以表示为：

```text
prepare_request
      |
      v
provider->chat
      |
      v
Assistant Message
      |
      +---- remember()
      |
      v
execute_tool_calls()
      |
      v
Tool Message(s) -> Memory
```

一个很值得注意的设计：

`step()` 不判断 Agent 是否完成。

它只是完成“一轮”。

是否退出由上层 `co_run()` 判断。

这是一种很干净的职责分离：

```text
step()
  = execute one round

co_run()
  = control repetition / termination
```

---

## 8. prepare_request()：每轮模型调用前做什么

**代码位置：** `src/agent.cpp` → `Agent::prepare_request`；声明见 `include/libagent/agent.hpp`


源码：

```cpp
boost::asio::awaitable<ChatRequest>
Agent::prepare_request() {

    co_await opts_.memory->compact();

    std::string context;

    if (opts_.retriever) {
        const std::string query =
            latest_user_query();

        if (!query.empty()) {
            auto chunks =
                co_await opts_.retriever->retrieve(
                    query,
                    opts_.rag_top_k
                );

            context = join_context(chunks);
        }
    }

    ChatRequest req;

    req.options = opts_.generate;

    req.options.tools =
        opts_.tools->schemas();

    req.messages =
        build_messages(context);

    co_return req;
}
```

顺序非常重要：

```text
Memory compact
      |
      v
get latest user query
      |
      v
Retriever.retrieve
      |
      v
ToolRegistry.schemas
      |
      v
build_messages
      |
      v
ChatRequest
```

这里有三个重要的架构结论。

### 8.1 Memory 可以在每一轮前维护上下文

```cpp
co_await opts_.memory->compact();
```

例如 SummarizingMemory 可以在这里：

```text
很长历史
   |
   v
LLM summary
   |
   v
短历史
```

Agent 不关心具体怎么压缩。

---

### 8.2 RAG 是 request preparation 的一部分

Retriever 并不参与 Agent 控制流程。

它只是：

```text
latest user query
      |
      v
Retriever
      |
      v
RetrievedChunk[]
      |
      v
System Prompt context
```

因此 RAG 是 prompt augmentation，而不是 Agent Loop 本身。

---

### 8.3 Tool schema 每轮都会交给模型

```cpp
req.options.tools = opts_.tools->schemas();
```

模型能够调用哪些工具，是通过 ToolDefinition / JSON Schema 告诉 Provider，再由 Provider 转成具体 API 格式。

---

## 9. build_messages()：最终给模型的消息

**代码位置：** `src/agent.cpp` → `Agent::build_messages`、`Agent::latest_user_query`


逻辑：

```text
system_prompt
     +
RAG context
     +
memory history
```

最终大约是：

```text
System:
  You are...

  Relevant context from retrieved documents:
  - ...
  - ...

User:
  ...

Assistant:
  ...

Tool:
  ...
```

值得注意：

`system_prompt` 默认不存储在 Memory 中，而是在每次 request 时动态插到最前面。

优点：

- 系统提示词与会话状态分离
- 修改 system prompt 不需要修改 Memory
- 避免 Memory 策略误删系统提示词

---

## 10. execute_tool()：单个工具如何运行

**代码位置：** `src/agent.cpp` → `Agent::execute_tool`；工具接口见 `include/libagent/tool.hpp`


核心：

```cpp
const Tool* tool =
    opts_.tools->find(tc.name);
```

如果没找到：

```json
{
  "error": "unknown tool: ..."
}
```

如果找到：

```cpp
out_json =
    co_await tool->handler(tc.arguments);
```

随后构造：

```cpp
Message result{Role::Tool};

result.tool_call_id = tc.id;
result.name = tc.name;
result.content.text = out_json.dump();
```

其中 `tool_call_id` 非常关键。

它负责告诉模型：

> 这个 Tool Result 对应前面哪个 ToolCall。

调用关系：

```text
ToolCall
  id = call_123
  name = weather
        |
        v
ToolRegistry::find("weather")
        |
        v
handler(arguments)
        |
        v
Json result
        |
        v
Message
  role = Tool
  tool_call_id = call_123
```

---

## 11. execute_tool_calls()：为什么不是简单 for 循环

**代码位置：** `src/agent.cpp` → `Agent::execute_tool_calls`


如果模型一次请求多个工具：

```text
weather()
maps()
hotel()
```

libagent 会并发启动。

核心结构：

```cpp
for (...) {
    boost::asio::co_spawn(
        ex,
        coroutine,
        boost::asio::detached
    );
}
```

每个 coroutine 完成后：

```cpp
co_await chan->async_send(..., i, ...);
```

主协程：

```cpp
idx =
    co_await chan->async_receive(...);
```

因此它是典型的：

```text
              +--> tool A --+
              |             |
Agent --------+--> tool B --+--> channel --> join
              |             |
              +--> tool C --+
```

也就是：

**fan-out / fan-in**

目的主要是优化 IO-bound Tool latency。

如果：

```text
weather = 500 ms
maps    = 700 ms
hotel   = 800 ms
```

串行理论耗时：

```text
500 + 700 + 800 = 2000 ms
```

并发理论上接近：

```text
max(500,700,800) ≈ 800 ms
```

---

## 12. 一个完整例子

假设用户：

```text
查询深圳天气，然后告诉我是否适合散步。
```

### Round 0

Memory：

```text
User:
查询深圳天气，然后告诉我是否适合散步。
```

prepare_request() 组装请求。

模型返回：

```text
Assistant:
tool_calls:
  weather(city="深圳")
```

Agent 保存 Assistant Message。

执行：

```text
weather("深圳")
```

返回：

```json
{
  "temperature": 28,
  "rain": false
}
```

写入 Memory：

```text
Tool:
{"temperature":28,"rain":false}
```

### Round 1

prepare_request() 再次构造：

```text
User:
查询深圳天气，然后告诉我是否适合散步。

Assistant:
tool_call weather(...)

Tool:
{"temperature":28,"rain":false}
```

再次调用 LLM。

这次返回：

```text
Assistant:
今天 28°C，无降雨，适合散步。
```

并且：

```cpp
tool_calls.empty() == true
```

于是：

```cpp
co_return resp.message.content.text;
```

Agent 结束。

---

## 13. 为什么叫 ReAct Loop

传统 ReAct 常描述为：

```text
Thought
Action
Observation
Thought
Action
Observation
...
Answer
```

libagent 并没有显式定义 `Thought` 类型。

它实际上实现的是：

```text
LLM reasoning
     |
     v
ToolCall        = Action
     |
     v
Tool Result     = Observation
     |
     v
LLM reasoning
     |
     v
Final answer
```

因此从工程角度，它更准确地说是：

> **基于 native tool calling 的 ReAct-style control loop。**

---

## 14. max_tool_rounds 为什么存在

**代码位置：** `include/libagent/agent.hpp` → `AgentOptions::max_tool_rounds`；使用位置见 `src/agent.cpp` → `Agent::co_run`


```cpp
int max_tool_rounds = 10;
```

防止：

```text
LLM -> Tool
       |
       v
LLM -> Tool
       |
       v
LLM -> Tool
       |
       ...
```

无限循环。

达到上限：

```text
[libagent] max_tool_rounds reached
```

这是 Agent Runtime 必须具备的安全边界之一。

生产系统通常还会进一步限制：

- 总 token budget
- 总 tool-call 数
- 每个 tool 的单次超时
- 总执行时间
- 总成本
- recursion depth
- 相同调用重复检测

这些是 libagent 当前可以继续加强的方向。

---

## 15. 面试官可能追问什么

### Q1：libagent 的 Agent Loop 是什么？

回答：

> Agent 收到用户输入后先写入 Memory，然后每轮通过 prepare_request 对 Memory 做 compact、可选执行 RAG、加入工具 schema 和上下文，之后调用 LLMProvider。模型响应被保存到 Memory。如果响应包含 tool_calls，Agent 会执行对应工具并把 Tool Result 作为 Tool Message 再写入 Memory，然后进入下一轮；如果没有 tool_calls，就认为得到最终答案并退出。

---

### Q2：为什么 Tool Result 要写回 Memory？

回答：

> 因为 Tool Result 本身并不是最终答案，而是下一轮模型推理需要的 observation。写回 conversation history 后，模型才能看到“之前调用了哪个工具，以及结果是什么”，从而继续推理。

---

### Q3：为什么 step() 不负责判断退出？

回答：

> `step()` 的职责是一轮完整的 LLM + Tool execution，而 `co_run()` 负责循环控制和退出判断。这使执行单元和控制策略解耦，代码更容易复用和测试。

---

### Q4：为什么 max_tool_rounds 必须存在？

回答：

> LLM 的工具选择是动态的，有可能反复调用工具甚至形成循环，因此 runtime 必须设置有限执行边界。libagent 用 max_tool_rounds 做最基本的 ReAct safety bound。

---

### Q5：这是完整意义上的 ReAct 吗？

回答：

> 它没有显式暴露 Thought 字段，而是利用现代 LLM 的 native tool calling 表达 Action，Tool Message 表达 Observation，所以更准确地说是 ReAct-style tool-calling loop。

---

## 16. 当前源码中值得注意的几个设计问题

### 16.1 退出主要依赖 tool_calls.empty()

当前：

```cpp
if (resp.message.tool_calls.empty()) {
    co_return resp.message.content.text;
}
```

并没有主要依赖 `FinishReason` 做状态控制。

这种设计简单，但未来如果 Provider 的异常 finish state 更复杂，可以考虑统一状态机处理。

### 16.2 max_tool_rounds 是 round bound，不是全局 budget

它只限制轮数。

没有直接限制：

- wall-clock time
- total tokens
- total tool calls
- monetary cost

生产 Agent 通常需要统一 Budget / ExecutionContext。

### 16.3 Tool Result 用 JSON dump 后塞进 text

当前统一性很好，但对于：

- 超大结果
- binary data
- structured artifacts

还可以设计 richer ToolResult abstraction。

---

## 17. 第一章需要记住的最小模型

只记这张图：

```text
User
 |
 v
remember(User)
 |
 v
+------------------------------------+
|              co_run                |
|                                    |
|   prepare_request                  |
|          |                         |
|          v                         |
|     provider->chat                 |
|          |                         |
|          v                         |
|   remember(Assistant)              |
|          |                         |
|     tool_calls ?                   |
|       /       \                    |
|     no         yes                 |
|     |           |                  |
|   return    execute tools          |
|                 |                  |
|                 v                  |
|          remember(Tool)            |
|                 |                  |
|                 +---- next round --+
+------------------------------------+
```

如果这张图完全理解，Agent Runtime 的主干就已经掌握了。
