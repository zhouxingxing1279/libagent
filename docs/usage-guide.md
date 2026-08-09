# libagent 使用指南

本指南面向"我要用这个库做某件事"。代码片段与 `include/libagent/` 中的真实 API 对齐,
可直接参考。所有示例都假设:

```cpp
namespace asio = boost::asio;
using namespace libagent;
```

执行模型要点:libagent 基于 C++20 协程(`boost::asio::awaitable<T>`),所有异步操作跑在
一个 `asio::io_context` 上,单线程即可、无需加锁。`Agent` 的协程入口通过 `co_spawn` 驱动。

---

## 目录
1. [最小可运行例子](#1-最小可运行例子)
2. [配置 Agent](#2-配置-agent)
3. [更改系统提示词](#3-更改系统提示词)
4. [增加一个工具](#4-增加一个工具)
5. [生成参数](#5-生成参数modeltemperaturemax_tokens)
6. [运行:阻塞 / 协程 / 流式](#6-运行阻塞--协程--流式)
7. [记忆:Full / Window / Summarizing](#7-记忆full--window--summarizing)
8. [RAG 检索增强](#8-rag-检索增强)
9. [多 Agent:HandoffRouter](#9-多-agenthandoffrouter)
10. [切换 / 新增 Provider](#10-切换--新增-provider)
11. [常见坑 / FAQ](#11-常见坑--faq)

---

## 1. 最小可运行例子

```cpp
asio::io_context ioc;

openai::Options opts;
opts.api_key = "sk-...";                  // 或设 base_url 用 DeepSeek/智谱/Ollama
auto provider = std::make_shared<openai::OpenAiProvider>(std::move(opts));

AgentOptions a;
a.provider = provider;
a.memory   = std::make_shared<FullMemory>();
a.system_prompt = "You are a concise assistant.";

Agent agent(a);

std::string reply = asio::co_spawn(
    ioc, agent.co_run("Say hello in one sentence."), asio::use_future).get();
ioc.run();
```

`Agent::co_run` 返回 `awaitable<std::string>`(最终回答文本);上面用 `use_future` + `get()`
阻塞取结果。`run(...)` 是这个模式的便捷封装,直接返回字符串。

---

## 2. 配置 Agent

`AgentOptions` 的全部字段:

| 字段 | 类型 | 说明 |
|---|---|---|
| `provider` | `shared_ptr<LLMProvider>` | LLM 后端(必填) |
| `memory` | `shared_ptr<Memory>` | 对话记忆(必填) |
| `tools` | `shared_ptr<ToolRegistry>` | 工具集(默认空) |
| `system_prompt` | `optional<string>` | 系统提示词,见 §3 |
| `retriever` | `shared_ptr<Retriever>` | RAG 检索器,见 §8 |
| `rag_top_k` | `int` (=3) | 每轮检索的片段数 |
| `generate` | `GenerateOptions` | 生成参数,见 §5 |
| `max_tool_rounds` | `int` (=10) | ReAct 安全上限 |

```cpp
AgentOptions a;
a.provider = provider;
a.memory   = std::make_shared<WindowMemory>(20);
a.tools    = std::make_shared<ToolRegistry>();
a.max_tool_rounds = 8;
Agent agent(a);
```

---

## 3. 更改系统提示词

设置 `system_prompt`,Agent 会在**每轮请求的最前面**注入一条 `Role::System` 消息
(不写进 memory,所以记忆里始终只有对话本身):

```cpp
a.system_prompt = "You are a SQL expert. Always answer with a SQL query unless asked otherwise.";
```

如果你希望系统提示词带动态信息,直接用字符串拼接即可。也可以**不设** `system_prompt`,
改由你自己往 `memory` 里放一条 `Role::System` 消息来管理。

> 与 RAG 同时使用时:检索到的上下文会拼到系统提示词后面,作为同一条 System 消息一起发送。

---

## 4. 增加一个工具

一个工具 = 一份 **JSON Schema 描述** + 一个**协程 handler**。handler 接收参数 `Json`,
返回 `awaitable<Json>`(结果),内部可 `co_await` 任何异步操作(子 HTTP、数据库、子 agent…)。

```cpp
Tool weather;
weather.spec.name        = "get_weather";
weather.spec.description = "Get the current weather for a city.";
weather.spec.parameters  = Json{
    {"type", "object"},
    {"properties", Json{
        {"city", Json{{"type", "string"}, {"description", "City name, e.g. Tokyo"}}}}},
    {"required", Json::array({"city"})}};

weather.handler = [](const Json& args) -> asio::awaitable<Json> {
    const std::string city = args.value("city", std::string{"unknown"});
    // 这里可以 co_await 真实网络/DB 调用;示例直接返回
    co_return Json{{"city", city}, {"temp_c", 21}, {"condition", "cloudy"}};
};

a.tools->add(std::move(weather));
```

要点:
- **参数 schema 必须是 `{"type":"object", ...}`**。若是无参工具,用 `Json{{"type","object"}}`
  即可(库会自动补 `type:object`,但显式写更清晰)。
- **handler 抛异常会被捕获**,自动转成 `Role::Tool` 的 `{"error": "..."}` 消息回灌给模型,
  模型可据此重试或改用别的工具——你无需自己处理异常。
- **未知工具名**同样转成 error 消息,不会让整轮崩溃。
- 一轮里模型发出多个 `tool_calls` 时,这些工具会**并发**执行(单线程 io_context 上协作交错);
  对网络密集型工具,这意味着多个调用重叠而非串行。

带工具后,典型对话:

```cpp
std::string reply = asio::co_spawn(
    ioc, agent.co_run("What's the weather in Tokyo?"), asio::use_future).get();
```

模型若决定调 `get_weather`,Agent 会执行它、把结果回灌,再让模型据此作答,直到给出最终回答
(或达到 `max_tool_rounds`)。

---

## 把命令行工具一行包成 Tool(`cli::command`)

`libagent/tools/cli.hpp` 提供 `cli::command(...)`,把任意外部程序包成一个 Tool——
程序直接 `exec`(不走 shell,无注入风险),`{占位符}` 从调用参数替换,自动捕获
`stdout` / `stderr` / 退出码。返回:

```json
{ "exit": 0, "stdout": "...", "stderr": "..." }   // 启动失败时: { "error": "..." }
```

```cpp
#include <libagent/tools/cli.hpp>
using namespace libagent::cli;

a.tools->add(command(
    "weather",                                    // 工具名(也是函数名)
    "Get the weather for a city",                 // 描述
    {{"city", "string"}},                         // 参数 {名 -> JSON 类型}
    "/usr/bin/curl",                              // 可执行程序
    {"-s", "https://wttr.in/{city}?format=3"}));  // argv 模板,{city} 会被替换
```

模型调用 `weather({"city":"Tokyo"})` 时,handler 会执行 `curl -s '...Tokyo...'` 并把输出回灌。
参数为空就传 `{}`;多参数照常 `{{"a","number"},{"b","string"}}`。

> **链接要求:** 该 header 依赖 Boost.Process v2(Boost ≥ 1.86),核心 libagent 不含此依赖。
> 用到它的工程需额外链接:
> ```cmake
> find_package(Boost CONFIG REQUIRED COMPONENTS headers process)
> target_link_libraries(my_app PRIVATE libagent::libagent Boost::process)
> ```

---

## 5. 生成参数(model / temperature / max_tokens)

通过 `AgentOptions::generate`(`GenerateOptions`):

```cpp
a.generate.model       = "deepseek-chat";   // 覆盖 provider 的 default_model
a.generate.temperature = 0.2;
a.generate.max_tokens  = 512;
a.generate.top_p       = 0.9;
a.generate.stop        = {"\n\n"};          // 可选 stop 序列
```

`model` 留空时用 provider 的 `default_model`。

---

## 6. 运行:阻塞 / 协程 / 流式

**阻塞便捷**(内部起一个 io_context):
```cpp
std::string reply = agent.run("Hello!");
```

**协程**(在你自己的 io_context 上驱动,适合嵌入异步代码):
```cpp
asio::co_spawn(ioc, [&] {
    return agent.co_run("Hello!");
}, asio::use_future).get();
```

**流式**(最终回答**逐 token 实时**推送到 sink;工具轮非流式,仅最终回答流式):
```cpp
asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
    TokenSink sink = [](const StreamEvent& ev) -> asio::awaitable<void> {
        if (ev.kind == StreamEvent::Kind::Delta) {
            std::cout << ev.delta << std::flush;        // 增量文本
        } else if (ev.kind == StreamEvent::Kind::Finish) {
            std::cout << "\n[finish=" << static_cast<int>(ev.finish) << "]\n";
        }
        co_return;                                       // awaitable sink = 真背压
    };
    co_await agent.co_run_stream("Tell me a short story.", sink);
}, asio::detached);
ioc.run();
```

`sink` 本身是 `awaitable<void>(const StreamEvent&)`,你 `co_await` 它之前协程会挂起——
所以慢消费者能对 provider 形成真实背压。

---

## 取消运行

agent 运行可被中途取消。协程形式:用 `bind_cancellation_slot` 派发 `co_run`,另一处
(或另一线程)emit 取消信号即可中断当前 LLM/工具调用:

```cpp
asio::cancellation_signal sig;
auto fut = asio::co_spawn(
    ioc, agent.co_run("long task"),
    asio::bind_cancellation_slot(sig.slot(), asio::use_future));
// ...稍后:
sig.emit(asio::cancellation_type::all);   // 中断;fut.get() 会抛 system_error
```

阻塞形式提供了接受取消槽的重载:

```cpp
asio::cancellation_signal sig;
// 另一线程可调 sig.emit(...) 中断:
std::string reply = agent.run("long task", sig.slot());
```

取消会传播到协程链里的所有 asio 操作(http、定时器等)。`cli::command` 启动的子进程在
取消时会被终止(不会成为孤儿进程)。

---

## 7. 记忆:Full / Window / Summarizing

- `FullMemory()` —— 完整对话,不截断(自己负责控制长度)。
- `WindowMemory(n)` —— 固定窗口;超容量时驱逐**最旧的非 System 消息**,始终保留系统提示。
- `SummarizingMemory(opts, inner)` —— 包装一个内层 memory(默认 FullMemory);当对话超过阈值,
  把旧轮次让 **provider 压缩成一条 System 摘要**,仅保留最近若干轮原文。

```cpp
SummarizingMemory::Options so;
so.provider        = provider;   // 用哪个 provider 做摘要
so.summarize_above = 12;         // 对话超过这么多条就开始压缩
so.keep_recent     = 6;          // 最近这么多条原文保留
auto mem = std::make_shared<SummarizingMemory>(so);
// 或自选内层:SummarizingMemory(so, std::make_shared<WindowMemory>(20));

a.memory = mem;
```

摘要由 Agent 每步自动触发(`Memory::compact()` 钩子,阈值以下为 no-op)。

---

## 8. RAG 检索增强

设置 `retriever` 后,Agent 每轮会按**最近一条 user 消息**检索 top-k 片段,作为上下文注入:

```cpp
auto retriever = std::make_shared<SimpleCorpusRetriever>();
retriever->add_document("libagent is a C++20 AI agent framework.");
retriever->add_document("Tools are JSON-Schema-described coroutines.");
// ...

a.retriever  = retriever;
a.rag_top_k  = 3;
```

`SimpleCorpusRetriever` 是零依赖、按词重叠排序的本地检索器,适合起步与测试;
要更强的语义检索,实现 `Retriever` 接口(`retrieve(query, k) -> awaitable<vector<RetrievedChunk>>`)
替换即可,Agent 代码无需改动。

---

## 9. 多 Agent:HandoffRouter

把若干子 agent 注册给一个 router;每个子 agent 被合成成一个**工具**,router 的 LLM
决定把任务委派给谁,子 agent 跑完自己的 ReAct 循环、把答案作为工具结果返回。

```cpp
auto researcher = std::make_shared<Agent>(researcher_opts);  // 各自的 provider/工具/记忆
auto coder      = std::make_shared<Agent>(coder_opts);

HandoffRouter router(
    planner_provider,                         // router 自己用的 LLM
    std::make_shared<FullMemory>(),           // router 的记忆
    {AgentHandle{"researcher", researcher},
     AgentHandle{"coder", coder}});

// router 本身就是一个 Agent
std::string reply = asio::co_spawn(
    ioc, router.agent().co_run("Find and then summarize..."), asio::use_future).get();
```

运行时还能 `router.add_agent({...})` 动态注册。

---

## 10. 切换 / 新增 Provider

### 用其它 OpenAI 兼容端点

只需改 `base_url`(host 可带端口):

```cpp
openai::Options opts;
opts.api_key = "sk-...";
opts.base_url       = "https://api.deepseek.com";   // 或智谱/Moonshot/vLLM/Ollama
opts.default_model  = "deepseek-chat";
```

Ollama 本地: `opts.base_url = "http://localhost:11434/v1"`(注意是 http,key 填任意值)。

### 写自己的 Provider

实现 `LLMProvider` 的两个方法即可,其余框架照常工作:

```cpp
class MyProvider : public LLMProvider {
public:
    asio::awaitable<ChatResponse> chat(const ChatRequest& req) override {
        // 组包 → 调你的 API → 解析成 ChatResponse
        ChatResponse out;
        out.message.role = Role::Assistant;
        out.message.content.text = "...";
        out.finish = FinishReason::Stop;
        co_return out;
    }

    asio::awaitable<void> stream(const ChatRequest& req, TokenSink sink) override {
        StreamEvent delta;
        delta.kind  = StreamEvent::Kind::Delta;
        delta.delta = "...";
        co_await sink(delta);

        StreamEvent fin;
        fin.kind = StreamEvent::Kind::Finish;
        // 若该轮有工具调用:fin.tool_calls = {...}
        co_await sink(fin);
    }
};

a.provider = std::make_shared<MyProvider>();
```

---

## 11. 常见坑 / FAQ

- **`co_run` 调了但没反应?** awaitable 必须 被 `co_spawn` 驱动在运行中的 io_context 上;
  裸构造一个 awaitable 不跑。最省事用阻塞的 `agent.run(...)`。
- **provider 返回 HTTP 400 "schema must be type: object"?** 工具参数 schema 需是
  `{"type":"object",...}`;库已对无 `type` 的空 schema 自动补 `object`,但建议显式写。
- **工具没被调用?** 多数是 system prompt 没引导模型用工具,或该 provider 对 function calling
  支持有限。可在 system prompt 明确"能回答就用工具"。
- **DeepSeek/Claude 等返回 `tool_calls` 时arguments 是字符串?** 库已兼容解析(字符串或对象均可)。
- **流式时工具轮为什么不流式?** 工具轮需要完整的 assistant 消息来解析 `tool_calls`,故非流式;
  仅最终回答逐 token 流式。这是有意设计。
- **多线程?** 当前按单线程 io_context 设计(无锁)。多线程需用 `strand` 包裹共享状态,留作后续。
- **密钥安全:** 不要把 API key 写进源码或提交。示例都从环境变量读(`LIBAGENT_API_KEY`)。
```
