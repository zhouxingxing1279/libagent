# 02. 核心数据模型 / Message Model

## 1. 这一章要解决什么问题

这一章回答：

> **libagent 内部不同模块之间，究竟通过什么数据结构进行通信？**

如果第一章解决的是“Agent 如何跑起来”，那么第二章解决的是：

> **Agent、Provider、Tool、Memory 之间传递的数据长什么样。**

libagent 没有让核心 Runtime 直接依赖 OpenAI / Anthropic 的原始 JSON，而是先定义一套统一的 provider-agnostic 数据模型。

核心类型：

```text
Role
Content
ImageRef
Message
ToolCall
ToolDefinition
GenerateOptions
ChatRequest
ChatResponse
FinishReason
Usage
```

---

## 2. 本章源码定位

| 主题 | 代码位置 | 关键符号 |
| --- | --- | --- |
| 核心值类型 | `include/libagent/types.hpp` | `Role`, `Message`, `ToolCall`, `ToolDefinition`, `GenerateOptions` |
| JSON 序列化 | `src/types.cpp` | `to_json`, `from_json` |
| Provider 请求/响应 | `include/libagent/provider.hpp` | `ChatRequest`, `ChatResponse`, `LLMProvider` |
| Agent 请求组装 | `src/agent.cpp` | `prepare_request`, `build_messages` |

---

## 3. 为什么需要统一数据模型

如果 Agent Runtime 直接使用某个厂商的数据结构：

```text
Agent
  |
  v
OpenAIMessage
OpenAIToolCall
OpenAIResponse
```

那么一旦换成 Anthropic、DeepSeek 或其他兼容接口，Agent 主循环、Memory、Tool Registry 都可能受到影响。

libagent 采用：

```text
                    libagent canonical model
                             |
            +----------------+----------------+
            |                                 |
            v                                 v
      OpenAIProvider                   AnthropicProvider
            |                                 |
            v                                 v
      OpenAI wire JSON                 Anthropic wire JSON
```

因此：

> **Provider 负责协议适配，Agent Runtime 只处理统一的数据模型。**

这是 libagent 最重要的解耦点之一。

---

## 4. Role：消息角色

**代码位置：** `include/libagent/types.hpp`

```cpp
enum class Role {
    System,
    User,
    Assistant,
    Tool
};
```

四种角色：

```text
System     系统提示词
User       用户输入
Assistant  LLM 输出
Tool       工具执行结果
```

普通对话：

```text
System
User
Assistant
```

Agent Tool Calling：

```text
User
Assistant(tool_calls)
Tool(result)
Assistant(final answer)
```

`Tool` 是关键，因为工具结果需要作为正式消息回灌到下一轮上下文。

C++ 语法见：

[enum class 笔记](../cpp-notes/03-enum-class.md)

---

## 5. Content / ImageRef：为什么 Message 不只是 string

**代码位置：** `include/libagent/types.hpp`

```cpp
struct ImageRef {
    std::string url;
    std::optional<std::string> media_type;
    std::optional<std::string> data;
};

struct Content {
    std::string text;
    std::vector<ImageRef> images;
};
```

如果只设计：

```cpp
std::string content;
```

就只能支持纯文本。

当前设计允许：

```text
Content
├── text
└── images[]
```

这为 multimodal provider 提供了统一入口。

`ImageRef` 同时保留：

- URL / data URI 风格
- media_type + base64 data 风格

因此核心 Runtime 不需要绑定某一家视觉 API。

---

## 6. ToolCall：模型提出的“工具调用请求”

**代码位置：** `include/libagent/types.hpp`

```cpp
struct ToolCall {
    std::string id;
    std::string name;
    Json arguments = Json::object();
};
```

含义：

```text
id         这次调用的唯一标识
name       要调用哪个工具
arguments  实际参数
```

例如：

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

需要特别注意：

> `ToolCall` 只是模型提出的调用请求，它本身不会执行任何函数。

真正执行发生在：

```text
Agent::execute_tool()
  -> ToolRegistry::find(name)
  -> tool->handler(arguments)
```

---

## 7. 为什么 ToolCall 必须有 id

一次 Assistant Message 可以同时返回多个 ToolCall：

```text
call_1 -> weather("深圳")
call_2 -> weather("北京")
```

执行完成后，模型需要知道每个 Tool Result 对应哪个请求。

因此：

```text
Assistant ToolCall
id = call_1
      ^
      |
Tool Message
tool_call_id = call_1
```

这是 request-response correlation。

和网络系统中的：

- request_id
- trace_id
- correlation_id

属于同一类设计思想。

---

## 8. Message：libagent 的统一消息协议

**代码位置：** `include/libagent/types.hpp`

```cpp
struct Message {
    Role role = Role::User;
    Content content;
    std::vector<ToolCall> tool_calls;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> name;
};
```

它不是“普通聊天字符串”，而是 Agent、Provider、Tool、Memory 之间的统一消息载体。

### 8.1 User Message

```cpp
Message{
    Role::User,
    Content{"10 * 3 是多少？"}
};
```

### 8.2 Assistant 普通回答

```text
role = Assistant
content.text = "结果是 30"
tool_calls = []
```

### 8.3 Assistant Tool Call

```text
role = Assistant
tool_calls = [
    {
        id = "call_123",
        name = "calculate",
        arguments = {...}
    }
]
```

### 8.4 Tool Result Message

```text
role = Tool
tool_call_id = "call_123"
name = "calculate"
content.text = "{\"result\":30}"
```

所以 Tool Calling 不是在 Runtime 外部旁路执行，而是正式进入 conversation history。

---

## 9. ToolDefinition：Agent 告诉模型“有什么工具”

**代码位置：** `include/libagent/types.hpp`

```cpp
struct ToolDefinition {
    std::string name;
    std::string description;
    Json parameters = Json::object();
};
```

它和 `ToolCall` 的方向相反。

```text
ToolDefinition
Agent -> LLM
"你可以调用什么"

ToolCall
LLM -> Agent
"我决定调用什么"
```

例如：

```json
{
  "name": "calculate",
  "description": "Evaluate a basic arithmetic expression.",
  "parameters": {
    "type": "object",
    "properties": {
      "a": {"type": "number"},
      "op": {
        "type": "string",
        "enum": ["+", "-", "*", "/"]
      },
      "b": {"type": "number"}
    },
    "required": ["a", "op", "b"]
  }
}
```

其中 `parameters` 是 JSON Schema。

它描述“参数应该是什么结构”，不是实际参数。

---

## 10. ToolDefinition 与 ToolCall 的区别

必须记住：

```text
ToolDefinition.parameters
    =
参数规范 / Schema

ToolCall.arguments
    =
实际参数
```

例如：

```text
Schema:
a 是 number
op 是 + - * /
b 是 number
```

模型真正返回：

```json
{
  "a": 10,
  "op": "*",
  "b": 3
}
```

---

## 11. GenerateOptions：一次模型生成的配置

**代码位置：** `include/libagent/types.hpp`

核心字段：

```cpp
struct GenerateOptions {
    std::optional<std::string> model;
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    std::optional<int> top_p;
    std::vector<std::string> stop;
    bool stream = false;
    std::vector<ToolDefinition> tools;
    std::optional<Json> response_format;
    std::optional<Json> tool_choice;
    std::optional<long> seed;
};
```

可以理解为：

```text
Message / messages
    =
告诉模型“上下文是什么”

GenerateOptions
    =
告诉模型“这次怎么生成”
```

其中 Tool Calling 最关键的是：

```cpp
std::vector<ToolDefinition> tools;
std::optional<Json> tool_choice;
```

`tools` 告诉模型候选工具。

`tool_choice` 控制：

- auto
- none
- required
- 指定具体函数

具体语义最终由 Provider 翻译到厂商 API。

---

## 12. ChatRequest：发给 Provider 的统一请求

**代码位置：** `include/libagent/provider.hpp`

```cpp
struct ChatRequest {
    std::vector<Message> messages;
    GenerateOptions options;
};
```

可以抽象为：

```text
ChatRequest
├── messages
│   └── 当前对话上下文
│
└── options
    └── model / temperature / tools / tool_choice ...
```

因此：

> 一次 LLM 请求 = conversation context + generation configuration。

---

## 13. ChatResponse：Provider 返回给 Agent 的统一结果

**代码位置：** `include/libagent/provider.hpp`

```cpp
struct ChatResponse {
    Message message;
    FinishReason finish = FinishReason::Stop;
    Usage usage;
    std::optional<Json> raw;
};
```

含义：

```text
message  模型生成的统一 Message
finish   为什么结束
usage    token 使用量
raw      可选原始 Provider payload
```

`raw` 主要用于 debugging / tracing，而 Runtime 应优先依赖统一字段。

---

## 14. FinishReason

**代码位置：** `include/libagent/types.hpp`

```cpp
enum class FinishReason {
    Stop,
    Length,
    ToolCalls,
    ContentFilter,
    Error
};
```

含义：

```text
Stop           正常结束
Length         token / 长度限制
ToolCalls      模型请求执行工具
ContentFilter  Provider 内容过滤
Error          错误
```

当前 Agent 主循环最终是否继续，主要看：

```cpp
resp.message.tool_calls.empty()
```

而不是只依赖 `FinishReason::ToolCalls`。

这意味着：

> 实际 ToolCall 数据是当前 Runtime 的主要控制依据。

---

## 15. Usage

```cpp
struct Usage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
};
```

当前主要是 metadata，但未来可以用于：

- token budget
- context compact trigger
- 成本统计
- 性能监控
- Agent 执行预算

当前项目的 `max_tool_rounds` 只限制 round 数，并不是完整 budget system。

---

## 16. types.cpp：统一对象与 JSON 的转换

**代码位置：** `src/types.cpp`

`types.hpp` 定义 C++ 数据结构。

`types.cpp` 负责：

```text
C++ canonical object
       <=>
canonical JSON
```

例如：

```cpp
void to_json(Json& j, const ToolCall& t);
void from_json(const Json& j, ToolCall& t);
```

这个 canonical JSON 主要服务于：

- persistence
- tests
- 内部统一表示

它仍然不是某个 Provider 的最终 wire format。

Provider 还需要做下一层转换。

---

## 17. ToolCall.arguments 为什么支持 string / object 两种输入

**代码位置：** `src/types.cpp` → `from_json(const Json&, ToolCall&)`

```cpp
if (a.is_string()) {
    t.arguments =
        Json::parse(a.get<std::string>(), nullptr, false);
} else {
    t.arguments = a;
}
```

原因：

有些 API 会直接返回：

```json
"arguments": {
  "city": "深圳"
}
```

而 OpenAI-compatible 协议中常见：

```json
"arguments": "{\"city\":\"深圳\"}"
```

后者是“字符串里的 JSON”。

因此 libagent 会统一解析成：

```json
{
  "city": "深圳"
}
```

最终 Runtime 只面对：

```cpp
Json ToolCall::arguments
```

这体现了 canonical model 的价值。

---

## 18. 数据模型与 Provider 的边界

不要把：

```text
libagent::ToolDefinition
```

误认为 OpenAI 原始 JSON。

libagent 内部是：

```json
{
  "name": "calculate",
  "description": "...",
  "parameters": {...}
}
```

OpenAI Provider 会进一步包装成类似：

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
Agent
  |
  v
ToolDefinition
  |
  v
Provider adapter
  |
  v
Vendor-specific JSON
```

---

## 19. 一次完整的数据流

```text
User input
    |
    v
Message{Role::User}
    |
    v
Memory
    |
    v
prepare_request()
    |
    +--> messages
    |
    +--> GenerateOptions.tools
    |
    v
ChatRequest
    |
    v
LLMProvider
    |
    v
vendor API
    |
    v
ChatResponse
    |
    v
Message{Role::Assistant}
    |
    +--> content
    |
    +--> tool_calls[]
              |
              v
         Agent executes
              |
              v
      Message{Role::Tool}
              |
              v
            Memory
```

---

## 20. 这一章最重要的架构结论

### 20.1 Message 是统一协议

Agent / Memory / Provider / Tool result 都围绕 `Message` 工作。

### 20.2 Provider 负责协议适配

Agent Runtime 不需要理解 OpenAI / Anthropic 各自完整 wire format。

### 20.3 ToolDefinition 与 ToolCall 是双向协议

```text
Agent -> LLM : ToolDefinition
LLM -> Agent : ToolCall
```

### 20.4 Tool Result 也是 Message

工具结果通过 `Role::Tool` 回灌，而不是旁路返回。

### 20.5 结构化不等于业务正确

即使 `arguments` 是合法 JSON，也可能：

- 缺字段
- 类型不符
- 参数值业务非法
- 工具名不存在

因此 Runtime 仍然需要校验和异常处理。

---

## 21. 当前实现值得注意的问题

### 21.1 Runtime 当前没有看到完整 JSON Schema validation

ToolDefinition 中虽然保存了 Schema：

```cpp
ToolDefinition::parameters
```

但当前 `Agent::execute_tool()` 的主路径是：

```cpp
tool->handler(tc.arguments)
```

没有在这一层显式做完整 schema validation。

因此当前更接近：

```text
Schema
  -> 主要用于约束 / 引导 LLM 输出

Runtime
  -> handler 自己承担一部分参数防御
```

生产级设计可以加入统一：

```text
JSON parse
 -> schema validate
 -> business validate
 -> authorization
 -> execute
```

### 21.2 Message.content 最终仍偏文本中心

Tool Result 当前通常：

```cpp
result.content.text = out_json.dump();
```

对于：

- 超大结构化结果
- binary
- file / artifact handle
- streaming tool result

未来可以设计 richer content / ToolResult abstraction。

---

## 22. 面试可能追问

### Q1：为什么不直接使用 OpenAI 的 Message 类型？

> 为了让 Agent Runtime 与模型厂商协议解耦。libagent 定义 canonical Message / ToolCall / ChatRequest / ChatResponse，Provider 负责转换成具体厂商 wire format，因此 Memory、Agent Loop 和 Tool Runtime 不需要绑定 OpenAI。

### Q2：ToolDefinition 和 ToolCall 有什么区别？

> ToolDefinition 是 Agent 发给模型的能力描述，包含工具名、描述和参数 JSON Schema；ToolCall 是模型返回给 Agent 的实际调用请求，包含 call id、tool name 和 arguments。

### Q3：为什么 Tool Result 还要带 tool_call_id？

> 因为一个 Assistant Message 可以请求多个工具，模型必须知道每个 Tool Result 对应前面的哪个 ToolCall，因此需要通过 id 做 correlation。

### Q4：JSON Schema 能保证参数绝对正确吗？

> 不能简单等价。Schema 能帮助模型生成结构化参数，也可由 Provider strict mode 或 Runtime validator 强化，但 Agent 仍应进行 JSON、Schema、业务和权限层面的校验。

### Q5：为什么 Message 里同时有 tool_calls 和 tool_call_id？

> `tool_calls` 用于 Assistant Message，表示模型发起的一个或多个调用；`tool_call_id` 用于 Tool Message，表示当前工具结果是在回答哪一个调用。

---

## 23. 面试可直接复述

> libagent 在 Provider 之上定义了一套统一的 canonical data model。核心是 Message，它统一承载 User、Assistant 和 Tool 消息；ToolDefinition 表示 Agent 提供给模型的工具能力和参数 Schema，而 ToolCall 表示模型返回的实际工具调用请求。ChatRequest 由 messages 和 GenerateOptions 组成，ChatResponse 则统一承载模型消息、finish reason 和 usage。具体 Provider 负责把这些内部类型翻译成 OpenAI 或 Anthropic 等厂商协议。这样 Agent Loop、Memory 和 Tool Runtime 都不依赖具体 LLM API，实现了比较清晰的 provider abstraction。
