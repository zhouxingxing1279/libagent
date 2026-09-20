# C++ 值类别、右值引用与移动语义

> 本文是独立的 C++ 语言笔记，不讨论 libagent 的业务逻辑。
>
> 项目中的实际使用案例见：[Agent Runtime / ReAct Loop](../interview-notes/01-agent-runtime.md)

## 1. 左值和右值先怎么理解

入门阶段可以先记：

- **左值（lvalue）**：有稳定身份、通常还能继续通过名字访问的对象。
- **右值（rvalue）**：临时结果，通常马上就会消失，因此资源往往可以被转移。

例如：

```cpp
int a = 10;
```

其中：

- `a` 是左值。
- `10` 是右值。

粗略判断时，可以想：

```cpp
a = 20;   // 可以
10 = 20;  // 不可以
```

但“能否出现在赋值号左边”只是入门辅助判断，并不是标准对左值的严格定义。

---

## 2. 为什么 C++ 要区分左值和右值

核心目的是支持高效的资源管理。

例如：

```cpp
std::string a = "hello";
std::string b = a;
```

这里 `a` 后面可能还要继续使用，因此 `b` 只能复制 `a` 的资源。

而：

```cpp
std::string b = std::move(a);
```

表示程序员允许把 `a` 的内部资源转移给 `b`。

对于拥有堆资源的类型，例如：

- `std::string`
- `std::vector`
- `std::shared_ptr`
- 很多用户自定义 RAII 类型

移动通常比深拷贝更便宜。

---

## 3. std::move 到底做了什么

`std::move` **本身不搬数据**。

它的作用本质上是进行类型转换，把一个表达式转换成可绑定到右值引用的值类别。

例如：

```cpp
std::string a = "hello";
std::string b = std::move(a);
```

可以理解成：

```text
a
  ↓
std::move(a)
  ↓
告诉编译器：允许把 a 当成“可被移动资源”的对象
  ↓
匹配 std::string 的移动构造函数
  ↓
真正的资源转移发生
```

因此：

> `std::move` 只是“允许移动”，真正怎么移动由目标类型的 move constructor / move assignment 决定。

---

## 4. 为什么有名字的变量是左值

这是移动语义里非常重要的一点。

例如：

```cpp
std::string&& x = std::string("hello");
```

虽然 `x` 的**类型**是：

```cpp
std::string&&
```

但是表达式：

```cpp
x
```

本身仍然是左值。

原因是它已经有了名字和身份。

所以：

```cpp
std::string y = x;
```

走的是 copy。

而：

```cpp
std::string y = std::move(x);
```

才会尝试走 move。

面试常见问题：

> 一个 `T&&` 类型的命名变量，是左值还是右值？

回答：

> 变量的类型可以是右值引用，但只要这个表达式有名字，表达式 `x` 本身就是左值。

---

## 5. 左值引用和右值引用

### 左值引用

```cpp
T&
```

通常绑定左值：

```cpp
int a = 10;
int& ref = a;
```

普通左值引用不能直接绑定字面量：

```cpp
int& ref = 10;  // error
```

### 右值引用

```cpp
T&&
```

可以绑定右值：

```cpp
int&& ref = 10;
```

右值引用是 C++11 移动语义的重要基础。

---

## 6. Copy Constructor 和 Move Constructor

一个典型资源类可能同时提供：

```cpp
class Buffer {
public:
    Buffer(const Buffer& other);  // copy constructor
    Buffer(Buffer&& other);       // move constructor
};
```

如果：

```cpp
Buffer a;
Buffer b = a;
```

因为 `a` 是左值，通常调用：

```cpp
Buffer(const Buffer&)
```

如果：

```cpp
Buffer b = std::move(a);
```

则 `std::move(a)` 可以匹配：

```cpp
Buffer(Buffer&&)
```

---

## 7. moved-from 对象还能不能用

可以继续析构，也可以重新赋值，但状态通常是：

> **valid but unspecified state**

例如：

```cpp
std::string a = "hello";
std::string b = std::move(a);
```

之后不能依赖：

```cpp
a == ""
```

一定成立。

但可以：

```cpp
a.clear();
a = "new value";
```

因为对象仍然是合法对象，只是具体内容不应被假设。

---

## 8. 按值接收 + move 到成员变量

现代 C++ 中经常看到：

```cpp
class A {
public:
    explicit A(Config config)
        : config_(std::move(config)) {}

private:
    Config config_;
};
```

这种写法适合：

> 构造函数最终一定要持有这个参数。

如果调用方传左值：

```cpp
Config config;
A a(config);
```

大致经历：

```text
config
  ↓ copy
形参 config
  ↓ move
成员 config_
```

如果调用方传右值：

```cpp
A a(Config{});
```

或者：

```cpp
A a(std::move(config));
```

就可以充分利用移动语义。

这类参数常被称为 **sink parameter**：函数接收参数后就是为了把它存下来。

---

## 9. 为什么优先使用成员初始化列表

推荐：

```cpp
A::A(Config config)
    : config_(std::move(config)) {}
```

而不是：

```cpp
A::A(Config config) {
    config_ = std::move(config);
}
```

前者是：

```text
直接构造成员 config_
```

后者通常是：

```text
先默认构造 config_
  ↓
再 move assignment
```

多了一步。

因此：

> 构造成员时应优先使用成员初始化列表。

---

## 10. 更严格的 C++ 值类别

完整体系包括：

```text
                 expression
                     |
          +----------+----------+
          |                     |
       glvalue                rvalue
       /    \                 /    \
   lvalue   xvalue         xvalue  prvalue
```

因此：

```text
rvalue = prvalue + xvalue
```

`std::move(a)` 严格来说产生的是 **xvalue**。

当前阶段准备面试时，优先掌握：

- lvalue
- rvalue
- `T&`
- `T&&`
- `std::move`
- copy constructor
- move constructor
- moved-from object

之后再继续深入 prvalue / xvalue / forwarding reference。

---

## 11. libagent 中的对应案例

项目代码位置：

```text
src/agent.cpp
Agent::Agent
```

代码：

```cpp
Agent::Agent(AgentOptions opts)
    : opts_(std::move(opts)) {}
```

这里：

1. `opts` 是一个有名字的函数参数，所以表达式 `opts` 是左值。
2. `std::move(opts)` 把它转换为可移动的值类别。
3. 成员 `opts_` 因此可以调用 `AgentOptions` 的移动构造。
4. 参数 `opts` 在构造函数结束后本来就会销毁，因此移动其资源很合理。

项目层面的设计说明见：

[Agent Runtime / ReAct Loop：AgentOptions](../interview-notes/01-agent-runtime.md#3-agentoptionsagent-的依赖注入入口)

---

## 12. 面试回答

### std::move 会执行移动吗？

不会。

`std::move` 只是把表达式转换成可匹配右值引用的值类别。真正资源转移发生在类型的移动构造函数或移动赋值运算符中。

### 为什么一个 T&& 变量仍然可能是左值？

因为“引用类型”和“表达式值类别”是两个概念。命名变量表达式具有稳定身份，所以表达式本身是左值。

### 为什么构造函数常写成按值接收再 move？

当函数最终需要持有参数时，这种写法同时兼容左值和右值调用，并能让右值路径利用移动语义，接口也相对简洁。
