# C++ 面试与源码语法笔记

这里专门记录阅读 libagent 时涉及的 **C++ 语言机制和标准库知识**。

项目架构、Agent Runtime、Tool、Memory、Provider 等业务与框架设计笔记仍放在：

[libagent 面试学习笔记](../interview-notes/README.md)

## 当前笔记

1. [值类别、右值引用与移动语义](01-value-categories-and-move-semantics.md)

后续预计补充：

- RAII 与智能指针
- `shared_ptr` / `unique_ptr`
- `std::function` 与 Lambda
- C++20 Coroutine
- `co_await` / `co_return`
- Boost.Asio Executor
- Future / Promise
- 模板与 Perfect Forwarding
- 异常安全
