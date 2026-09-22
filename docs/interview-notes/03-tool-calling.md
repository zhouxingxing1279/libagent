# 03. Tool Calling / 工具调用执行链

## 1. 这一章要解决什么问题

这一章回答：

> **LLM 知道有哪些工具以后，libagent 是如何完成“选择 -> 查找 -> 执行 -> 回灌 -> 再推理”的？**

完整链路：

```text
定义 Tool
 -> 注册 ToolRegistry
 -> 提取 ToolDefinition
 -> Provider 发送给 LLM
 -> LLM 返回 ToolCall
 -> Agent 按 name 查 Registry
 -> handler(arguments)
 -> Tool Message
 -> Memory
 -> next LLM round
```

---

## 2. 本章源码定位

| 主题 | 代码位置 | 关键符号 |
| --- | --- | --- |
| Tool 抽象 | `include/libagent/tool.hpp` | `ToolHandler`, `Tool`, `ToolRegistry` |
| Registry 实现 | `src/tool.cpp` | `add`, `find`, `schemas`, `remove` |
| 请求时暴露工具 | `src/agent.cpp` | `prepare_request` |
| 单 Tool 执行 | `src/agent.cpp` | `execute_tool` |
| 多 Tool 并发 | `src/agent.cpp` | `execute_tool_calls` |
| 示例 | `examples/02_tool_agent.cpp` | `calculate`, `get_current_time` |
| Registry 测试 | `tests/test_tool_registry.cpp` | `AddFindRemove`, `SchemasAndProviderSchema` |

---

## 3. Tool 的核心设计：spec + handler

**代码位置：** `include/libagent/tool.hpp`

```cpp
struct Tool {
    ToolDefinition spec;
    ToolHandler handler;
};
```

一个 Tool 被拆成两部分：

```text
Tool
├── spec
│   └── 给 LLM 看的接口描述
│
└── handler
    └── Runtime 真正执行的 C++ callable
```

例如计算器：

```text
spec
  name = calculate
  description = 基本算术
  parameters = a/op/b JSON Schema

handler
  calculate(const Json& args)
```

这是非常关键的边界：

> **LLM 不会看到 C++ 函数实现，它只看到 ToolDefinition。**

---

## 4. ToolHandler：统一所有工具执行接口

**代码位置：** `include/libagent/tool.hpp`

```cpp
using ToolHandler =
    std::function<
        boost::asio::awaitable<Json>(const Json&)
    >;
```

业务含义：

```text
input:
  Json arguments

output:
  async Json result
```

抽象成普通函数就是：

```cpp
Json tool(Json args);
```

libagent 使用异步版本：

```cpp
awaitable<Json> tool(const Json& args);
```

这样：

- HTTP 工具
- 数据库工具
- 文件工具
- RPC 工具
- 本地计算工具

都可以暴露成同一种 handler interface。

C++ 的 `std::function`、lambda、函数对象属于语言知识，建议单独放到 cpp-notes，不在业务笔记里展开。

---

## 5. 示例：calculate 工具

**代码位置：** `examples/02_tool_agent.cpp`

```cpp
boost::asio::awaitable<libagent::Json>
calculate(const libagent::Json& args) {
    const double a = args.value("a", 0.0);
    const double b = args.value("b", 0.0);
    const std::string op =
        args.value("op", std::string{"+"});

    double r = 0.0;

    if (op == "+") {
        r = a + b;
    } else if (op == "-") {
        r = a - b;
    } else if (op == "*") {
        r = a * b;
    } else if (op == "/") {
        r = (b != 0.0 ? a / b : 0.0);
    }

    co_return Json{{"result", r}};
}
```

如果 ToolCall.arguments 为：

```json
{
  "a": 10,
  "op": "*",
  "b": 3
}
```

handler 最终返回：

```json
{
  "result": 30
}
```

---

## 6. 工具如何注册

**代码位置：** `examples/02_tool_agent.cpp`

创建：

```cpp
auto tools =
    std::make_shared<ToolRegistry>();
```

注册：

```cpp
tools->add({
    {
        "calculate",
        "Evaluate a basic arithmetic expression.",
        Json{
            {"type", "object"},
            {"properties",
             Json{
                 {"a", Json{{"type", "number"}}},
                 {"op", Json{
                     {"type", "string"},
                     {"enum", Json::array({"+", "-", "*", "/"})}
                 }},
                 {"b", Json{{"type", "number"}}}
             }},
            {"required", Json::array({"a", "op", "b"})}
        }
    },
    calculate
});
```

展开后等价于：

```text
Tool
├── spec.name = "calculate"
├── spec.description = "..."
├── spec.parameters = JSON Schema
└── handler = calculate
```

---

## 7. ToolRegistry 本质是什么

**代码位置：** `include/libagent/tool.hpp`

```cpp
std::unordered_map<std::string, Tool> tools_;
```

所以本质是：

```text
name -> Tool
```

例如：

```text
"calculate"
    ->
Tool{
  spec,
  handler=calculate
}

"get_current_time"
    ->
Tool{
  spec,
  handler=get_current_time
}
```

LLM 最终返回的是字符串工具名，因此 Runtime 需要一个 name -> executable object 的映射。

---

## 8. add()：注册工具

**代码位置：** `src/tool.cpp`

```cpp
void ToolRegistry::add(Tool t) {
    auto name = t.spec.name;
    tools_.emplace(
        std::move(name),
        std::move(t)
    );
}
```

注册以后：

```text
tools_["calculate"] = Tool(...)
```

后续 Agent 通过 ToolCall.name 查找。

---

## 9. schemas()：只把工具接口暴露给 LLM

**代码位置：** `src/tool.cpp`

```cpp
std::vector<ToolDefinition>
ToolRegistry::schemas() const {
    std::vector<ToolDefinition> out;
    out.reserve(tools_.size());

    for (const auto& kv : tools_) {
        out.push_back(kv.second.spec);
    }

    return out;
}
```

这里非常重要：

> **只提取 Tool.spec，不发送 handler。**

```text
ToolRegistry
   |
   +-- Tool.spec ----------> LLM
   |
   +-- Tool.handler
          |
          +---------------> stays local
```

因此 LLM 只知道：

- 工具叫什么
- 什么时候适合使用
- 参数格式是什么

不知道函数内部如何实现。

---

## 10. prepare_request()：每轮都把工具定义交给模型

**代码位置：** `src/agent.cpp`

```cpp
ChatRequest req;
req.options = opts_.generate;

req.options.tools =
    opts_.tools->schemas();

req.messages =
    build_messages(context);
```

这句：

```cpp
req.options.tools = opts_.tools->schemas();
```

就是：

> **LLM 为什么知道 Agent 有什么工具的源码答案。**

每一轮模型调用前，可用 ToolDefinition 都会进入 ChatRequest。

---

## 11. Provider 的职责

Agent 只生成：

```text
vector<ToolDefinition>
```

Provider 再把它转为具体模型 API 所要求的格式。

例如 OpenAI-compatible 风格通常会包装为：

```json
{
  "type": "function",
  "function": {
    "name": "calculate",
    "description": "...",
    "parameters": {...}
  }
}
```

因此：

```text
ToolRegistry
    |
    v
ToolDefinition
    |
    v
GenerateOptions.tools
    |
    v
Provider
    |
    v
vendor-specific tools JSON
```

---

## 12. LLM 到底负责什么

模型负责两件事：

```text
1. 是否需要调用工具
2. 如果需要：
   选择 name
   生成 arguments
```

例如返回：

```json
{
  "id": "call_123",
  "name": "calculate",
  "arguments": {
    "a": 10,
    "op": "*",
    "b": 3
  }
}
```

模型并不会直接进入你的进程调用：

```cpp
calculate(...)
```

它只产生一个结构化 ToolCall。

所以 Tool Calling 是：

```text
LLM:
  select + describe action

Agent Runtime:
  validate + dispatch + execute
```

---

## 13. step()：收到 ToolCall 后进入执行阶段

**代码位置：** `src/agent.cpp`

```cpp
ChatResponse resp =
    co_await opts_.provider->chat(req);

remember(resp.message);

co_await execute_tool_calls(
    resp.message.tool_calls
);
```

顺序：

```text
Provider response
   |
   v
remember Assistant Message
   |
   v
read message.tool_calls
   |
   v
execute_tool_calls()
```

为什么先保存 Assistant Message？

因为下一轮模型需要完整看到：

```text
Assistant:
  我刚才发起了 call_123

Tool:
  call_123 的结果是 ...
```

两条消息必须成对存在。

---

## 14. execute_tool()：按名字找到真正函数

**代码位置：** `src/agent.cpp`

```cpp
const Tool* tool =
    opts_.tools->find(tc.name);
```

假设：

```text
tc.name = "calculate"
```

那么：

```text
ToolCall.name
    |
    v
ToolRegistry::find("calculate")
    |
    v
Tool*
```

这一步就是 dispatcher。

---

## 15. find()：name -> Tool

**代码位置：** `src/tool.cpp`

```cpp
const Tool*
ToolRegistry::find(
    const std::string& name
) const {
    const auto it = tools_.find(name);

    return it == tools_.end()
        ? nullptr
        : &it->second;
}
```

模型输出的是字符串：

```text
"calculate"
```

Registry 负责把它解析成：

```text
真实 Tool object
```

因此 LLM 与本地函数之间存在一个受控映射层，而不是任意函数调用。

---

## 16. 真正执行 Tool 的代码

**代码位置：** `src/agent.cpp`

```cpp
out_json =
    co_await tool->handler(
        tc.arguments
    );
```

假设：

```text
tool->handler = calculate
```

语义上相当于：

```cpp
out_json =
    co_await calculate(
        tc.arguments
    );
```

因此：

```text
ToolCall
  name = calculate
  arguments = {...}
      |
      v
Registry lookup
      |
      v
Tool.handler
      |
      v
calculate(arguments)
      |
      v
Json result
```

---

## 17. 未知工具如何处理

如果 LLM 返回：

```text
name = "not_registered"
```

则：

```cpp
if (tool == nullptr) {
    out_json =
        Json{
            {"error",
             "unknown tool: " + tc.name}
        };
}
```

不会尝试执行不存在的函数。

这是一个重要安全边界：

> **LLM 只能请求 Registry 已注册的工具。**

---

## 18. Tool 执行异常如何处理

```cpp
try {
    out_json =
        co_await tool->handler(tc.arguments);
} catch (const std::exception& e) {
    out_json =
        Json{{"error", e.what()}};
}
```

Tool 失败不会直接让 Agent 进程崩掉，而是被转换成 observation：

```json
{
  "error": "..."
}
```

下一轮 LLM 可以：

- 修正参数
- 换工具
- 重试
- 告诉用户失败

因此：

> **Tool error 也是一种 Tool Result。**

---

## 19. Tool Result 为什么必须变成 Message

工具返回：

```json
{
  "result": 30
}
```

Agent 构造：

```cpp
Message result{Role::Tool};

result.tool_call_id = tc.id;
result.name = tc.name;
result.content.text = out_json.dump();
```

形成：

```text
Role::Tool
tool_call_id = call_123
name = calculate
content = {"result":30}
```

原因：

> Tool Result 不是最终用户回答，而是下一轮 LLM 推理的 observation。

---

## 20. 为什么 Tool Result 不能直接返回给用户

例如天气 API 返回：

```json
{
  "temp": 28.3,
  "rain_probability": 0.14,
  "wind_speed": 3.2
}
```

用户真正需要的可能是：

```text
今天约 28°C，降雨概率较低，适合外出。
```

职责分离：

```text
Tool
  -> 提供事实 / 外部能力

LLM
  -> 结合用户问题解释结果
```

所以执行后需要进入下一轮模型推理。

---

## 21. Tool Result 如何回灌 Memory

**代码位置：** `src/agent.cpp` → `execute_tool_calls`

工具完成后：

```cpp
remember(
    std::move((*results)[idx])
);
```

Memory 变为：

```text
User
  "10 * 3 是多少？"

Assistant
  ToolCall call_123
  calculate(...)

Tool
  tool_call_id = call_123
  {"result":30}
```

下一轮 `prepare_request()` 会重新读取 history，于是 LLM 可以继续回答。

---

## 22. 为什么 ToolCall 可以一次多个

`Message` 中：

```cpp
std::vector<ToolCall> tool_calls;
```

不是：

```cpp
std::optional<ToolCall>
```

因为模型可能一次请求：

```text
weather("深圳")
weather("北京")
calculate(...)
```

libagent 为这种情况实现了 fan-out / fan-in。

---

## 23. execute_tool_calls()：并发执行多个工具

**代码位置：** `src/agent.cpp`

整体：

```text
              +--> Tool A --+
              |             |
Agent --------+--> Tool B --+--> join --> remember
              |             |
              +--> Tool C --+
```

核心使用：

- `co_spawn`
- `detached`
- `experimental::channel`

每个 Tool coroutine 独立启动，完成后通过 channel 把自己的 index 发回来。

这样 IO-bound tool 可以重叠执行。

如果：

```text
A = 500 ms
B = 700 ms
C = 800 ms
```

串行大约：

```text
2000 ms
```

并发理想情况接近：

```text
800 ms + scheduling overhead
```

详细 coroutine / Asio 机制放到第四章学习。

---

## 24. ToolCall 与 ReAct 的关系

传统 ReAct：

```text
Thought
Action
Observation
Thought
Action
Observation
Answer
```

libagent：

```text
LLM internal reasoning
    |
    v
ToolCall        = Action
    |
    v
Tool execution
    |
    v
Tool Message    = Observation
    |
    v
next LLM round
    |
    v
Final Answer
```

所以 libagent 更准确地说是：

> **native tool calling 驱动的 ReAct-style loop。**

---

## 25. Tool Calling 的完整 10 步

### Step 1：定义 handler

```cpp
awaitable<Json>
calculate(const Json& args);
```

### Step 2：创建 ToolDefinition

```text
name
description
parameters schema
```

### Step 3：组成 Tool

```text
Tool = spec + handler
```

### Step 4：注册到 ToolRegistry

```text
"calculate" -> Tool
```

### Step 5：prepare_request() 提取 schemas

```cpp
req.options.tools =
    opts_.tools->schemas();
```

### Step 6：Provider 发给模型

Provider 翻译为厂商 tool calling 协议。

### Step 7：LLM 返回 ToolCall

```text
id
name
arguments
```

### Step 8：Agent dispatcher 查找

```cpp
opts_.tools->find(tc.name)
```

### Step 9：执行 handler

```cpp
co_await tool->handler(tc.arguments)
```

### Step 10：Tool Message 回灌并进入下一轮

```text
result -> Role::Tool -> Memory -> LLM
```

---

## 26. 一个完整例子

用户：

```text
10 * 3 是多少？
```

### Round 0

Agent 发送：

```text
messages:
  User: 10 * 3 是多少？

tools:
  calculate(a, op, b)
```

模型返回：

```text
Assistant:
  ToolCall
    id = call_123
    name = calculate
    arguments = {
      a: 10,
      op: "*",
      b: 3
    }
```

Runtime：

```text
find("calculate")
   |
   v
handler = calculate
   |
   v
calculate(arguments)
   |
   v
{"result":30}
```

Memory：

```text
User
Assistant(ToolCall)
Tool({"result":30})
```

### Round 1

LLM 再看到完整上下文，返回：

```text
Assistant:
10 × 3 = 30。
```

此时：

```cpp
tool_calls.empty() == true
```

Agent 结束。

---

## 27. LLM 如何“选择”工具

LLM 看到的是：

- name
- description
- JSON Schema
- 当前 messages
- tool_choice

例如：

```text
get_current_time
  Get the current UTC time.

calculate
  Evaluate a basic arithmetic expression.
```

用户问：

```text
现在 UTC 时间是多少？
```

模型根据上下文和 ToolDefinition，生成：

```text
get_current_time
```

Runtime 本身并没有硬编码：

```cpp
if (user says "time") {
    use get_current_time;
}
```

工具选择主要由模型完成。

---

## 28. Native Tool Calling 与“让模型输出 JSON”不同

不要把这两件事混为一谈。

普通 prompt：

```text
请严格输出：
{"tool":"calculate", ...}
```

这是：

```text
LLM 普通文本
 -> 自己提取 JSON
 -> 自己判断是不是工具请求
```

Native Tool Calling：

```text
LLM API
 -> 专门的 tool_calls 字段
 -> Provider parser
 -> libagent::ToolCall
```

后者协议更稳定，也更适合 Agent Runtime。

---

## 29. “结构化”不等于“可信”

即使模型返回：

```json
{
  "name": "calculate",
  "arguments": {
    "a": 10,
    "op": "*",
    "b": 3
  }
}
```

Runtime 仍不能默认：

- name 一定存在
- JSON 一定合法
- arguments 一定满足 Schema
- 参数业务值一定合理
- 该用户一定有权限调用

因此生产级执行链建议：

```text
ToolCall
  |
  v
tool exists?
  |
  v
JSON parse
  |
  v
schema validation
  |
  v
business validation
  |
  v
authorization / policy
  |
  v
timeout / budget
  |
  v
handler
```

当前 libagent 已有：

- unknown tool 检查
- exception catch
- max_tool_rounds
- max_tool_output_bytes

但仍有可增强空间。

---

## 30. 当前实现的几个重要边界

### 30.1 没有看到统一 schema validator

`ToolDefinition.parameters` 已存在，但 `execute_tool()` 目前直接：

```cpp
tool->handler(tc.arguments)
```

因此 schema validation 不是 Runtime 强制步骤。

### 30.2 没有独立的 Tool timeout

如果某个 handler 长时间不返回，当前单 Tool 执行路径没有看到统一 timeout wrapper。

### 30.3 Tool permission / sandbox 还比较轻

Registry 决定“能不能找到”，但生产环境通常还需要：

- user/session permission
- filesystem sandbox
- network allowlist
- destructive action confirmation
- rate limit

### 30.4 Tool Result 统一序列化为文本

```cpp
result.content.text =
    out_json.dump();
```

简单统一，但 richer artifact 可以继续抽象。

---

## 31. 面试可能追问

### Q1：LLM 怎么知道有哪些工具？

> Agent 在每轮 prepare_request 时调用 ToolRegistry::schemas()，把注册工具的 ToolDefinition 放进 GenerateOptions.tools，Provider 再转换成模型 API 对应的 tools 字段。因此模型看到的是工具名、描述和参数 Schema，不会看到本地 handler 实现。

### Q2：LLM 是怎么真正调用 C++ 函数的？

> 它并不会直接调用 C++。模型只返回 ToolCall{name, arguments}，Agent 根据 name 在 ToolRegistry 中查到 Tool，再调用对应的 handler(arguments)。

### Q3：为什么 Tool 要拆成 spec 和 handler？

> spec 是模型侧能力描述，handler 是本地执行实现。拆开以后模型协议和执行逻辑解耦，而且 Runtime 可以只向模型暴露接口，不泄露实现。

### Q4：如果模型返回不存在的工具怎么办？

> Agent::execute_tool 会通过 ToolRegistry::find 查找，找不到就生成一个 error Tool Result，而不是执行任意函数。

### Q5：为什么工具结果还要交给 LLM？

> 工具结果是 observation，不一定适合直接面向用户。把结果作为 Role::Tool Message 回灌后，模型可以结合原问题解释、整合多个工具结果或继续调用下一个工具。

### Q6：为什么 ToolHandler 返回 awaitable<Json>？

> 因为很多真实工具本质是异步 IO，例如 HTTP、RPC、数据库和文件操作。统一成 awaitable<Json> 后可以直接进入 Agent 的 Asio coroutine runtime，并允许多个 IO-bound ToolCall 并发执行。

### Q7：这个设计的安全边界在哪里？

> LLM 只能提出 ToolCall，真正执行由 Registry 控制。当前实现已经检查 unknown tool、捕获 handler exception，并限制 round/output；生产级系统还应增加 schema validation、权限、timeout、budget 和 sandbox。

---

## 32. 面试可直接复述

> libagent 的 Tool Calling 被拆成声明和执行两层。Tool 由 ToolDefinition 和 ToolHandler 组成，前者包含 name、description 和 JSON Schema，提供给 LLM；后者是真正的本地异步 C++ callable。所有 Tool 注册到 ToolRegistry，内部通过 unordered_map 建立 name 到 Tool 的映射。每轮 prepare_request 会把 Registry 的 schemas 放到 GenerateOptions.tools，Provider 再转换成模型 API 的工具协议。模型如果返回 ToolCall，Agent 根据 ToolCall.name 在 Registry 查找并执行 handler(arguments)，随后将 JSON 结果包装为带 tool_call_id 的 Role::Tool Message 写回 Memory，再进入下一轮模型推理。因此模型负责选择和生成参数，Runtime 负责受控分发、真实执行和结果回灌。
