# Boost.Asio 中的 io_context、co_spawn、use_future 与同步包装

> 本文只讲 C++ / Boost.Asio 执行模型。
>
> libagent 中的实际业务使用见：
> [Agent Runtime / ReAct Loop：run()](../interview-notes/01-agent-runtime.md#4-外部入口run)

## 1. 对应代码

项目位置：

```text
src/agent.cpp
Agent::run(std::string user_input)
```

代码：

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

这段代码的作用是：

> 把一个返回 `boost::asio::awaitable<std::string>` 的异步协程，包装成一个对外同步返回 `std::string` 的普通函数。

---

## 2. 异步编程基础：先分清几个概念

在理解 Boost.Asio 之前，先区分下面几个概念。

### 2.1 同步

同步表示：当前操作没完成，调用流程不会继续往下走。

例如：

```cpp
auto result = request_llm();
process(result);
```

如果 `request_llm()` 需要 5 秒，那么当前线程通常会一直等到它返回。

```text
发请求
  ↓
等待 5 秒
  ↓
收到结果
  ↓
继续执行
```

### 2.2 阻塞

阻塞描述的是**线程状态**。

如果线程调用一个阻塞操作，那么在结果回来之前，这个线程不能继续执行其他代码。

所以要注意：

> 同步/异步描述的是调用关系；阻塞/非阻塞描述的是线程是否被占住。

两组概念相关，但不是完全等价。

### 2.3 异步

异步的核心思想是：

> 某个任务在等待 IO 时，先把控制权还给调度器，让线程去推进其他可执行任务。

例如 Agent 同时等待：

```text
LLM 网络请求
天气 API
地图 API
数据库
```

如果每个等待都阻塞线程，会浪费大量时间。

异步模型允许：

```text
任务 A 发请求
  ↓
等待 IO，先挂起 A

任务 B 开始执行
  ↓
等待 IO，先挂起 B

任务 C 继续执行

A 的网络结果回来
  ↓
恢复 A
```

### 2.4 并发与并行

**并发（concurrency）**表示多个任务在同一个时间区间内都能持续推进。

```text
A 做一点
B 做一点
A 等 IO
B 继续
```

**并行（parallelism）**表示多个任务在同一时刻真的同时执行。

```text
CPU 核 1：执行 A
CPU 核 2：执行 B
```

因此：

> 单线程程序也可以通过事件循环和协程实现并发，但不能实现真正的 CPU 并行执行。

### 2.5 为什么 Agent 特别适合异步

Agent 大量操作都是 IO-bound：

- 请求 LLM
- 调用 Tool HTTP API
- 数据库查询
- 文件 IO
- timer
- RPC

这些操作的大部分时间不是 CPU 在计算，而是在等待外部系统。

所以 Agent Runtime 很适合使用：

```text
event loop + coroutine + async IO
```

而不是每个任务都新建一个线程去阻塞等待。

---

## 3. Coroutine：可以暂停和恢复的函数

普通函数通常是：

```text
进入函数
  ↓
一直执行
  ↓
return
```

Coroutine 则可以：

```text
开始执行
  ↓
运行一段
  ↓
co_await
  ↓
保存状态并暂停
  ↓
稍后恢复
  ↓
继续执行
```

保存的状态通常包括：

- 当前执行位置
- 局部变量
- 参数
- coroutine frame 中的其他状态

因此可以把 coroutine 简单理解为：

> **可以被暂停、以后再从原位置继续执行的函数。**

### 3.1 co_await 的关键区别

普通阻塞等待：

```cpp
auto result = slow_function();
```

如果耗时 5 秒：

```text
线程被占住 5 秒
```

异步 coroutine：

```cpp
auto result = co_await async_function();
```

如果结果尚未准备好：

```text
当前 coroutine 暂停
  ↓
线程回到 event loop
  ↓
执行其他 ready task
  ↓
结果准备好
  ↓
coroutine 恢复
```

必须记住：

> **co_await 挂起的是 coroutine，不等于阻塞整个线程。**

### 3.2 为什么 coroutine 比 callback 容易读

传统 callback 异步代码可能写成：

```cpp
request_llm([](auto llm) {
    request_weather([llm](auto weather) {
        query_database([llm, weather](auto data) {
            // ...
        });
    });
});
```

容易形成 callback hell。

Coroutine 可以写成：

```cpp
auto llm = co_await request_llm();
auto weather = co_await request_weather();
auto data = co_await query_database();
```

逻辑看起来仍像同步代码，但底层可以异步挂起。

---

## 4. Event Loop：谁负责调度这些 coroutine

有了可以暂停的 coroutine，还需要一个调度中心。

Event Loop 可以粗略理解为：

```cpp
while (还有任务) {
    找一个现在可以继续执行的任务;
    执行它;
}
```

它需要管理：

- ready task
- 网络 IO 完成事件
- timer
- coroutine continuation

在 Boost.Asio 中，`io_context` 就承担了这个角色。

## 5. io_context 是什么

`boost::asio::io_context` 可以理解为 Asio 的事件循环和任务调度中心。

它负责驱动：

- 异步网络 IO
- timer
- coroutine continuation
- 通过 `post/dispatch/co_spawn` 提交的任务

可以粗略理解为：

```text
                  io_context
                     |
         +-----------+-----------+
         |           |           |
      task A      task B      IO completion
         |           |           |
         +-----------+-----------+
                     |
                  run()
```

仅仅创建：

```cpp
boost::asio::io_context ioc;
```

并不会自动执行任务。

真正驱动事件循环的是：

```cpp
ioc.run();
```

---

## 6. co_spawn 是什么

`co_spawn` 用来把一个 Asio coroutine 放到某个 executor 上运行。

例如：

```cpp
auto fut = boost::asio::co_spawn(
    ioc,
    co_run(...),
    boost::asio::use_future
);
```

三部分分别是：

```text
ioc
= 在哪个执行环境运行

co_run(...)
= 要运行的协程

use_future
= 协程结束以后，怎么把结果交还给调用者
```

所以可以理解为：

```text
co_run()
   |
   v
co_spawn
   |
   v
注册到 ioc 的 executor
```

---

## 7. awaitable<T> 是什么

libagent 的：

```cpp
boost::asio::awaitable<std::string>
Agent::co_run(...)
```

不是立即返回最终字符串。

它表示：

> 这是一个可以挂起和恢复的异步计算，最终会产生一个 `std::string`。

因此：

```cpp
co_run(...)
```

得到的是 coroutine/awaitable 对象，而不是最终答案。

真正执行它，需要通过：

```cpp
co_await co_run(...)
```

或者：

```cpp
co_spawn(...)
```

把它交给 Asio executor。

---

## 8. use_future 做了什么

`boost::asio::use_future` 是一个 CompletionToken。

它告诉 Asio：

> 当这个异步操作完成后，请把结果包装成 `std::future` 返回给我。

所以：

```cpp
auto fut = boost::asio::co_spawn(
    ioc,
    co_run(...),
    boost::asio::use_future
);
```

这里 `fut` 的概念类型可以理解为：

```cpp
std::future<std::string>
```

执行关系：

```text
co_run()
   |
   v
最终 co_return std::string
   |
   v
co_spawn completion
   |
   v
std::future<std::string>
```

---

## 9. 为什么必须 ioc.run()

这一句：

```cpp
ioc.run();
```

是真正驱动协程执行的地方。

如果只有：

```cpp
auto fut = co_spawn(...);
```

但不运行 `io_context`，事件循环没有线程驱动，协程以及它内部等待的异步操作就无法正常向前推进。

可以理解为：

```text
co_spawn
= 把任务放到调度系统里

ioc.run()
= 真正启动调度系统
```

在当前实现中，`ioc.run()` 由调用 `Agent::run()` 的这个线程执行。

因此默认情况下：

> 这个内部 `io_context` 是由当前线程单线程驱动的。

---

## 10. ioc.run() 是不是阻塞函数

是。

```cpp
ioc.run();
```

会占住当前线程，直到：

- 当前 io_context 没有剩余需要执行的 work，或者
- 被显式 stop

在 libagent 这个场景里：

```text
run()
  |
  v
ioc.run()
  |
  | 驱动整个 Agent coroutine
  |
  |-- LLM HTTP
  |-- Tool IO
  |-- coroutine resume
  |
  v
任务结束
```

所以虽然 Agent 内部实现是异步 coroutine：

```text
内部：async
```

但 `run()` 对调用者表现为：

```text
外部：blocking / synchronous
```

---

## 11. future.get() 在这里为什么通常不会再次长时间阻塞

代码：

```cpp
ioc.run();
return fut.get();
```

注意顺序。

先：

```cpp
ioc.run();
```

把整个协程驱动到完成。

再：

```cpp
fut.get();
```

取结果。

因此正常情况下，到 `future.get()` 时 future 已经 ready。

这里的 `get()` 主要做两件事：

1. 取出 `std::string` 结果。
2. 如果协程最终以异常结束，把异常重新抛到当前同步调用栈。

所以这个设计还顺便完成了：

```text
coroutine exception
        |
        v
std::future
        |
        v
future.get()
        |
        v
同步抛给 Agent::run 调用者
```

---

## 12. 为什么不直接 fut.get()

例如这样：

```cpp
auto fut = co_spawn(ioc, co_run(...), use_future);
return fut.get();
```

在当前设计中通常会出问题。

原因是：

- `future.get()` 在等待结果。
- 但是没有线程在执行 `ioc.run()`。
- coroutine 没有人驱动。
- future 又等着 coroutine 完成。

于是形成：

```text
当前线程：
future.get()
   |
   | 等待 coroutine
   v

coroutine：
等待 io_context 被 run
   |
   v

没人 run io_context
```

本质上就是无法推进。

因此这里必须有线程负责执行：

```cpp
ioc.run();
```

当前实现选择的就是调用线程自己执行。

---

## 13. 为什么 run() 仍然有价值

既然项目已经有：

```cpp
co_run()
```

为什么还提供：

```cpp
run()
```

因为不是所有调用者都运行在 coroutine 环境中。

如果外部程序只是普通：

```cpp
int main() {
    ...
}
```

那么调用：

```cpp
std::string answer = agent.run("hello");
```

明显比要求用户自己写：

```cpp
io_context
co_spawn
use_future
run
get
```

更方便。

所以它属于：

> **blocking convenience wrapper**

也就是“同步便利接口”。

---

## 14. run() 与 co_run() 的接口分层

```text
               Agent
          +------+------+
          |             |
       run()          co_run()
          |             |
     同步调用者      异步调用者
          |             |
     internal ioc    caller executor
          |
       co_spawn
          |
       co_run()
```

因此二者不是重复实现。

`co_run()`：

- 核心异步 API
- 可以嵌入更大的 Asio application
- 不创建自己的 event loop

`run()`：

- 同步适配器
- 自己创建 `io_context`
- 阻塞到任务完成

---

## 15. 这种设计的局限

当前：

```cpp
Agent::run()
```

每调用一次都会创建：

```cpp
boost::asio::io_context ioc;
```

这意味着它更适合：

- CLI
- 示例程序
- 简单同步程序
- 单次调用

如果是高并发服务器，更合理的是：

```text
进程级共享 io_context
        |
        +-- Agent A co_run
        +-- Agent B co_run
        +-- Agent C co_run
```

而不是每个请求：

```text
new io_context
run
destroy
```

高并发服务通常直接使用 `co_run()`。

---

## 16. 和线程是什么关系

默认这段：

```cpp
boost::asio::io_context ioc;
ioc.run();
```

只有一个调用线程执行 `run()`。

所以这个 io_context 默认是单线程执行。

但 coroutine 在遇到真正的异步 IO：

```cpp
co_await async_operation(...)
```

时会挂起当前 coroutine，让事件循环去处理其他 ready work。

因此：

```text
单线程
!=
只能串行等待所有 IO
```

而是可以有：

```text
Coroutine A -- await network ------+
                                   |
Coroutine B -- await timer ----+   |
                               |   |
Coroutine C -------- runnable  |   |
                               v   v
                          Event Loop
```

这叫 cooperative asynchronous concurrency。

---

## 17. 面试高频问题

### Q1：run() 明明内部是异步，为什么它还是同步接口？

因为它内部自己创建 `io_context` 并在当前线程调用 `ioc.run()`，一直驱动 coroutine 到结束后才返回。因此实现机制是异步的，但调用语义是阻塞同步的。

### Q2：co_spawn 的作用是什么？

将一个 Asio coroutine 提交到指定 executor 上执行，并通过 CompletionToken 决定异步操作完成后的结果交付形式。

### Q3：use_future 有什么作用？

它把异步完成结果转换为 `std::future`，让同步代码可以通过 `future.get()` 获取返回值或重新抛出 coroutine 中的异常。

### Q4：为什么不能只 future.get() 而不 ioc.run()？

因为 future 等待 coroutine 完成，而 coroutine 又需要 io_context 的 event loop 驱动。如果没有任何线程调用 `run()`，异步任务无法向前推进。

### Q5：这里会新建线程吗？

不会。创建 `io_context` 和 `co_spawn` 本身不会创建线程。当前实现是调用 `Agent::run()` 的线程执行 `ioc.run()`。

---

## 18. 一句话总结

```cpp
Agent::run()
```

本质上是：

> **创建一个临时 Asio event loop，把核心 coroutine `co_run()` 放进去运行，再通过 `std::future` 把 coroutine 的返回值和异常桥接回普通同步 C++ 调用栈。**


---

## 19. 完整执行时序

以：

```cpp
agent.run("帮我查天气");
```

为例：

```text
主线程
  |
  v
Agent::run
  |
  | 创建 io_context
  |
  | co_spawn(co_run)
  |
  v
ioc.run()
  |
  v
co_run 开始
  |
  v
step()
  |
  v
provider->chat()
  |
  | 发 HTTP 请求
  |
  v
co_await
  |
  | coroutine 暂停
  | event loop 可以推进其他任务
  |
  v
HTTP 返回
  |
  v
coroutine 恢复
  |
  v
模型要求调用 Tool
  |
  v
Tool coroutine
  |
  | 再次等待 IO
  |
  v
Tool 完成
  |
  v
下一轮 LLM
  |
  v
最终答案
  |
  v
co_return string
  |
  v
future ready
  |
  v
ioc.run() 返回
  |
  v
future.get()
  |
  v
Agent::run 返回 std::string
```

---

## 20. 当前阶段需要掌握的最小知识集

| 概念 | 当前阶段先这样理解 |
| --- | --- |
| 同步 | 当前操作没结束，调用流程继续等 |
| 异步 | 当前任务等待时，可以先推进其他任务 |
| 阻塞 | 线程被当前操作占住 |
| 并发 | 多个任务在同一时间区间内交替推进 |
| 并行 | 多个任务同一时刻真正同时执行 |
| coroutine | 可以暂停、恢复的函数 |
| `co_await` | 挂起当前 coroutine，等待异步结果 |
| `io_context` | Event Loop / 调度中心 |
| executor | 决定任务在哪里、按什么规则执行 |
| `co_spawn` | 把 coroutine 提交到执行环境 |
| `ioc.run()` | 当前线程开始驱动 event loop |
| `use_future` | 把异步结果桥接为 `std::future` |

学习顺序建议：

```text
同步/阻塞
  ↓
异步
  ↓
并发/并行
  ↓
coroutine
  ↓
co_await
  ↓
event loop
  ↓
io_context
  ↓
executor
  ↓
co_spawn
  ↓
use_future
```
