# libagent 路线图

框架核心已功能完整(40 测试绿、关键路径实网验证)。本文件列出后续完善与开发方向,
按"对真实使用的影响"分四档优先级,**P0 是上生产前应补的真实缺口**。

- 优先级:P0(生产加固)/ P1(功能扩展)/ P2(工程质量)/ P3(DX 与文档)
- 工作量:S(小)/ M(中)/ L(大)
- 状态:☐ 待办 / ☑ 已完成

---

## P0 — 生产加固(上生产前应做)

### ☑ 1. 网络超时 — `M`
- **完成**:重写 `https_client` 基于 `beast::tcp_stream` + `expires_after`。
  一次性请求对各阶段(connect/handshake/write/read)设超时;流式为每 chunk 重置的"无进展超时"。
  超时中止当前操作并以 `libagent: request timed out after Xms` 抛出。`Request.timeout` 恢复(默认 30s)。
  新增 stalled-server 超时测试。

### ☑ 2. 重试 + 指数退避 — `M`
- **完成**(非流式 `chat()`):`OpenAiProvider::Options` 加 `max_retries` / `initial_backoff` /
  `backoff_multiplier` / `max_backoff`。瞬时错误(超时/连接/HTTP 408·429·5xx)按指数退避重试,
  尊重 `Retry-After` 头。新增 3 个测试(429→重试成功、400 不重试立即抛、503 耗尽重试后抛)。
- **待续**:`stream()` 的重试需重构 SSE 回调(不能中途安全重发),留作后续。

### ☑ 3. 取消机制 — `M`
- **完成**:agent 运行可中途取消——协程形式用 `bind_cancellation_slot` 派发 `co_run` 即可
  (asio 取消自动传播到 http/定时器);新增阻塞重载 `run(input, cancellation_slot)`。
  `cli::command` 重写为取消安全:pipe/process 改 shared_ptr(避免取消展开时悬空),
  RAII guard 在取消/异常时 `terminate()` 杀子进程,取消经 `cancellation_state` 判定后传播。
  新增 2 个测试(agent 取消、cli 取消传播+杀子进程)。

### ☑ 4. 统一错误类型 — `M`
- **完成**:新增 `libagent::Error`(继承 `std::runtime_error`,向后兼容)+ `ErrorCode` 枚举
  (`Timeout`/`Network`/`Auth`/`RateLimited`/`Http`/`Provider`/...) + `http_status()`。在边界抛出
  有类型错误:https_client(超时→Timeout、传输→Network,取消仍以 operation_aborted 传播)、
  openai provider(HTTP 状态分类、malformed→Provider)。新增 2 个类型化测试。
- **待续**:完整的 `Result<T>`(把 `awaitable<T>` 改成返回 `Result<T>`)是侵入式重构、收益有限,
  暂缓;有类型的异常已满足"程序化区分错误"的需求。

### ☑ 5. 日志 + 可观测钩子 — `M`
- **完成**:`AgentOptions.hooks`(`on_message`/`on_llm_call`/`on_tool_call`,带 `steady_clock` 耗时)
  + `AgentOptions.log`(`LogSink`,见 `logging.hpp`)。agent 运行时在 LLM 调用、工具执行、消息持久化处
  触发钩子与日志。spdlog 作为**可选 adapter**(`logging/spdlog.hpp`,`make_spdlog_sink()`,
  CMake `-DLIBAGENT_WITH_SPDLOG=ON` 门控)——核心库不依赖 spdlog,默认构建精简。
  新增 hooks 测试 + 2 个 spdlog adapter 测试。

### ☑ 6. 工具输出大小限制 — `S`
- **完成**:`AgentOptions.max_tool_output_bytes`(默认 0=不限)。`execute_tool` 对超长工具结果
  截断并追加 `...[truncated by libagent: N bytes total]`,保护上下文窗口(覆盖所有工具,含 `cli::command`)。
  新增截断测试。

### ☑ 7. 跨平台 TLS 证书 — `S`
- **完成**:`https_client` 在 `set_default_verify_paths()` 之外,额外尝试加载一组常见 CA bundle
  路径(macOS Homebrew、`/etc/ssl/cert.pem`、Debian `ca-certificates.crt`、RHEL `ca-bundle.crt` 等),
  提高跨平台证书验证可靠性(OpenSSL 默认路径在某些 macOS/Windows 配置下为空)。49/49 离线测试绿;
  实网回归通过(`examples/hello_agent` 对 DeepSeek HTTPS 成功)。
- **后续**:内置 Mozilla CA bundle 兜底 / 自定义 CA 路径(corporate MITM)留作可选增强。

---

## P1 — 功能扩展

### ☑ 8. 第二个 provider(Anthropic Claude)— `M`
- **完成**:`include/libagent/providers/anthropic.hpp` + 实现,实现 `LLMProvider`(chat + stream)。
  处理 Claude 全部差异:system 顶层字段、content 块数组、`tool_use`/`tool_result` 块、`input_schema`、
  连续工具结果合并为单个 user 轮、`stop_reason`/`usage` 映射、SSE 流式(`content_block_*`/`message_delta`)。
  顺手把重试逻辑抽成共享 `http::send_with_retry`(OpenAI/Anthropic 共用)。4 个离线测试(text/tool_use/
  system 顶层/工具结果合并)。抽象跨厂商验证通过。
- **待人工验证**:真实 Anthropic API 需 Anthropic key(实网)。

### ☑ 9. EmbeddingProvider + 向量 RAG — `M`
- **完成**:`include/libagent/embedding.hpp`(`EmbeddingProvider` 接口)+
  `include/libagent/retrievers/vector.hpp`(`VectorRetriever`:余弦相似度 top-k,header-only)+ `chunk_text()`
  分块 + `include/libagent/providers/openai_embeddings.hpp`(`openai::Embedder`,OpenAI 兼容 embeddings 端点)。
  3 个离线测试(VectorRetriever 排序、分块、embedder 解析)。

### ☑ 10. 结构化输出 — `S`
- **完成**:`GenerateOptions` 加 `response_format`(JSON mode/json_schema,`optional<Json>` 直传)、
  `tool_choice`(`"auto"`/`"none"`/`"required"` 或具体函数对象)、`seed`。OpenAI provider 序列化这三项;
  `types.cpp` 的规范序列化也覆盖。新增序列化测试。Anthropic 的 tool_choice 格式不同,留作后续。

### ☑ 11. 多模态 Content — `M`
- **完成**:`Content` 加可选 `images`(`ImageRef`:url + 可选 media_type/base64),纯文本保持 text 快路径不变。
  OpenAI provider 发 `image_url` 块;Anthropic provider 发 `image` 块(base64 优先,否则 url source)。
  `types.cpp` 规范序列化覆盖。新增 OpenAI + Anthropic 各 1 个多模态测试。

### ☑ 12. 记忆持久化 — `M`
- **完成**:`include/libagent/memory_io.hpp`(header-only):`serialize(Memory)`/`load(Memory, Json)`
  + `save_to_file`/`load_from_file`。复用 Message 的 JSON 序列化,适用于任何 Memory 实现(Full/Window/Summarizing)。
  不改动 Memory 接口。新增 JSON 往返 + 文件往返 2 个测试。

### ☑ 13. AgentBus(多 agent 异步消息)— `M`
- **完成**:`include/libagent/agent_bus.hpp`(header-only):每个注册 agent 一个缓冲
  `channel<void(error_code, Message)>`;`post`/`await_message`/`close`。补充了 Phase 4 暂缓的异步消息
  能力(与 `HandoffRouter` 的同步委托互补)。3 个测试(收发、缓冲顺序、close 退出)。

### ☑ 14. 多线程 io_context — `M`
- **完成**:确认框架全程使用 `this_coro::executor`(strand 感知),**已可在多线程 io_context 上运行**
  ——每个 agent 跑在各自 strand 上即可(strand 串行化其协程)。新增多线程测试(8 agent × 各自 strand × 4 线程,
  全部正确完成)。文档说明多线程模型与"共享可变状态须放共享 strand"。

### ☐ 15. 更多开箱即用工具 — `S/M`
- **范围**:沿 `cli::command` 思路做 `http_get`、`sqlite_query`、`web_search` 等常用工具 helper。

---

## P2 — 工程质量 / CI

### ☐ 16. CI 真正跑起来 — `S`
- **现状**:`.github/workflows/ci.yml` 已写但无远程、从未执行。
- **范围**:加 GitHub remote 推送;验证三平台(尤其 Windows/vcpkg 分支);加 API-key 夜夜集成 job(可选)。

### ☐ 17. Sanitizers + 模糊测试 — `M`
- **范围**:ASan/UBSan 构建选项;对 SSE 解析、JSON 反序列化加 fuzz 目标(libFuzzer),覆盖畸形/恶意输入。

### ☐ 18. 基准测试 — `M`
- **范围**:provider 往返延迟、agent 循环开销、工具并发的 wall-clock 收益(验证并发 fan-out 的实际效果)。

### ☐ 19. 共享库 ABI / SO 版本 — `S`
- **范围**:若支持动态库,加 `SOVERSION` + 符号版本控制 + ABI 检查。

### ☐ 20. 打包 manifest — `S`
- **范围**:vcpkg / Conan manifest,便于他人 `find_package` 消费。

---

## P3 — DX / 文档

### ☐ 21. Doxygen 发布 — `S`
- **范围**:CI 生成 Doxygen 并发布到 GitHub Pages。

### ☐ 22. Cookbook — `S`
- **范围**:常用工具配方集(天气/搜索/数据库/文件)与典型 agent 编排示例。

### ☐ 23. Builder API — `S`
- **范围**:`AgentOptions` 流式/链式构造,减少样板。

---

## 建议推进顺序

**目标 = 尽快上生产**(性价比最高):
1. P0-1 网络超时 → P0-2 重试/退避(堵住"会挂死"的最大风险)
2. P0-5 日志 + 钩子(把现成 spdlog 用起来,可观测性立竿见影)
3. P0-3 取消 + P0-4 统一错误类型
4. P2-16 推 CI 验证三平台
5. P0-6/7 收尾(输出限制、证书)

**目标 = 扩展能力面**:
- P1-8 第二个 provider → P1-9 向量 RAG → P1-10 结构化输出 → P1-11 多模态

**目标 = 规模化 / 工程成熟**:
- P1-14 多线程 → P2-17/18 sanitizer + 基准 → P2-19/20 ABI + 打包

---

## 已完成基线(对照)
- ☑ Phase 0 构建系统 + install/export 卫生
- ☑ Phase 1 核心抽象(类型/序列化/provider 接口/tool/memory)
- ☑ Phase 2 协程 HTTPS + OpenAI 兼容 provider(chat + SSE)
- ☑ Phase 3 Agent ReAct 运行时
- ☑ Phase 4 多 agent 编排(`HandoffRouter`)
- ☑ Phase 5 记忆摘要 + RAG;并发工具 fan-out;实时逐 token 流式
- ☑ `cli::command` 命令行工具 helper
- ☑ 文档(README、使用指南、Doxygen 目标)、CI workflow
