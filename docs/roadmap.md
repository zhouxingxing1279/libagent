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

### ☐ 3. 取消机制 — `M`
- **现状**:无 `cancellation_signal`,长跑的 agent 无法中途叫停。
- **目标**:可取消运行中的 agent。
- **范围**:`Agent::co_run`/`run` 接受 `asio::cancellation_signal`;协程响应 cancel;`cli::command` 的子进程可被 kill。
- **涉及**:`agent.{hpp,cpp}`、`provider.hpp`、`tools/cli.hpp`

### ☐ 4. 统一错误类型 — `M`
- **现状**:异常 + ad-hoc `{"error":...}` 混用,调用方难程序化处理。
- **目标**:一致的错误模型。
- **范围**:定义 `libagent::Error{Code, message}` + `Result<T>`(或 `std::expected`);provider/agent/tool 统一使用;保留异常路径兼容。
- **涉及**:新增 `error.hpp`;横切适配

### ☐ 5. 日志 + 可观测钩子 — `M`
- **现状**:`spdlog` 是 PRIVATE 依赖却**完全没用**,靠 `std::cerr`。
- **目标**:结构化日志 + 运行时钩子。
- **范围**:接上 spdlog;定义 `LogSink`;`AgentOptions` 加 `hooks{on_llm_call,on_tool_call,on_message}`;记录请求/响应/耗时/token 用量。
- **涉及**:新增 `logging.hpp`/`hooks`;`agent.cpp`、`openai_provider.cpp`

### ☐ 6. 工具输出大小限制 — `S`
- **现状**:工具输出无截断,大输出可能撑爆上下文窗口。
- **范围**:`AgentOptions.max_tool_output_bytes`;超长截断并标注;`cli::command` 加 `max_output`。
- **涉及**:`tool.hpp`、`agent.cpp`、`tools/cli.hpp`

### ☐ 7. 跨平台 TLS 证书 — `S`
- **现状**:macOS 上 `set_default_verify_paths()` 不含 Keychain。
- **范围**:macOS 指向 Homebrew `cert.pem`;可选内置 Mozilla CA bundle 兜底;文档说明 Windows 用 vcpkg 的 CA。
- **涉及**:`https_client.cpp`、文档

---

## P1 — 功能扩展

### ☐ 8. 第二个 provider(Anthropic Claude)— `M`
- **目标**:验证 `LLMProvider` 抽象跨厂商;Claude Messages API(消息/工具格式与 OpenAI 不同)。

### ☐ 9. EmbeddingProvider + 向量 RAG — `M`
- **现状**:`SimpleCorpusRetriever` 仅关键词重叠。
- **范围**:`EmbeddingProvider` 接口;基于向量的 `Retriever`(本地 HNSW 或外部);文档切块(chunking)。

### ☐ 10. 结构化输出 — `S`
- **范围**:`GenerateOptions` 加 `response_format`(JSON mode)、`tool_choice`(auto/none/required/specific)、`seed`;provider 序列化。

### ☐ 11. 多模态 Content — `M`
- **现状**:`Content` 仅文本。
- **范围**:扩展 `Content` 支持 `image_url` 等;provider 适配视觉。

### ☐ 12. 记忆持久化 — `M`
- **范围**:`Memory` 加 save/load(磁盘/DB),支持跨会话;可序列化 `Message`(已有 JSON 序列化基础)。

### ☐ 13. AgentBus(多 agent 异步消息)— `M`
- **现状**:Phase 4 暂缓;`HandoffRouter` 只做同步委派。
- **范围**:基于 `asio::experimental::channel` 的 per-agent FIFO,实现真正的异步 agent 间消息传递。

### ☐ 14. 多线程 io_context — `M`
- **现状**:仅单线程,无锁。
- **范围**:用 `strand` 包裹共享状态;`co_spawn(strand, ...)`;并发工具执行与多线程统一。

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
