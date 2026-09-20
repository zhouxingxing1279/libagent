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

## 2. io_context 是什么

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

## 3. co_spawn 是什么

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

## 4. awaitable<T> 是什么

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

## 5. use_future 做了什么

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

## 6. 为什么必须 ioc.run()

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

## 7. ioc.run() 是不是阻塞函数

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

## 8. future.get() 在这里为什么通常不会再次长时间阻塞

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

## 9. 为什么不直接 fut.get()

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

## 10. 为什么 run() 仍然有价值

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

## 11. run() 与 co_run() 的接口分层

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

## 12. 这种设计的局限

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

## 13. 和线程是什么关系

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

## 14. 面试高频问题

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

## 15. 一句话总结

```cpp
Agent::run()
```

本质上是：

> **创建一个临时 Asio event loop，把核心 coroutine `co_run()` 放进去运行，再通过 `std::future` 把 coroutine 的返回值和异常桥接回普通同步 C++ 调用栈。**
