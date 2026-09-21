# C++ 强类型枚举 enum class

> 本文只记录 C++ 语法与语言机制。
>
> libagent 中的实际使用位置：
> `include/libagent/types.hpp` → `enum class Role`

## 1. 基本语法

```cpp
enum class Role {
    System,
    User,
    Assistant,
    Tool
};
```

它定义了一个名为 `Role` 的新类型，这个类型只能取有限几个枚举值。

使用：

```cpp
Role role = Role::User;
```

其中：

- `Role` 是枚举类型。
- `Role::User` 是一个枚举值。

---

## 2. 为什么要写 Role::User

`enum class` 是 scoped enum，也就是“有作用域的枚举”。

因此不能直接写：

```cpp
Role role = User;  // 错误
```

必须写：

```cpp
Role role = Role::User;
```

这样可以避免名字冲突。

例如：

```cpp
enum class Role {
    User
};

enum class AccountType {
    User
};
```

是合法的。

因为完整名称分别是：

```cpp
Role::User
AccountType::User
```

---

## 3. enum class 和传统 enum 的区别

传统枚举：

```cpp
enum Role {
    System,
    User,
    Assistant,
    Tool
};
```

可以直接：

```cpp
Role r = User;
```

这会让枚举值名字进入外层作用域，更容易冲突。

而：

```cpp
enum class Role {
    System,
    User,
    Assistant,
    Tool
};
```

需要：

```cpp
Role r = Role::User;
```

作用域更清晰。

---

## 4. enum class 是强类型枚举

传统 `enum` 通常可以隐式转换成整数：

```cpp
enum Color {
    Red,
    Green,
    Blue
};

int x = Red;
```

而：

```cpp
enum class Color {
    Red,
    Green,
    Blue
};
```

不能直接：

```cpp
int x = Color::Red;  // 错误
```

如果确实需要整数值，要显式转换：

```cpp
int x = static_cast<int>(Color::Red);
```

这就是“strongly typed”的意义之一。

---

## 5. 不同 enum class 之间不能随便比较

例如：

```cpp
enum class Role {
    User
};

enum class Status {
    User
};

Role r = Role::User;
Status s = Status::User;
```

下面这种比较是不允许的：

```cpp
if (r == s) {
}
```

因为 `Role` 和 `Status` 是两个不同的类型。

这能避免很多逻辑错误。

---

## 6. 默认底层值

如果没有显式赋值：

```cpp
enum class Role {
    System,
    User,
    Assistant,
    Tool
};
```

通常对应：

```text
System    = 0
User      = 1
Assistant = 2
Tool      = 3
```

可以通过：

```cpp
static_cast<int>(Role::Assistant)
```

得到对应整数值。

不过业务代码不应该无意中依赖这些默认编号。

---

## 7. 可以显式指定枚举值

```cpp
enum class ErrorCode {
    Success = 0,
    NetworkError = 100,
    Timeout = 101,
    Unknown = 999
};
```

使用：

```cpp
ErrorCode code = ErrorCode::Timeout;
```

显式转整数：

```cpp
int value = static_cast<int>(code);
```

结果是：

```text
101
```

---

## 8. 可以指定底层整数类型

完整语法可以写成：

```cpp
enum class Role : int {
    System,
    User,
    Assistant,
    Tool
};
```

也可以：

```cpp
#include <cstdint>

enum class Role : std::uint8_t {
    System,
    User,
    Assistant,
    Tool
};
```

这里：

```cpp
: std::uint8_t
```

表示底层存储类型。

---

## 9. enum class 很适合表示有限状态

如果某个变量只能有固定几种状态，枚举通常比字符串更合适。

例如：

```cpp
std::string role;
```

调用者可能写：

```cpp
role = "user";
role = "User";
role = "usr";
role = "abc";
```

编译器无法保证合法性。

而：

```cpp
Role role;
```

只能赋：

```cpp
Role::System
Role::User
Role::Assistant
Role::Tool
```

因此类型更安全。

---

## 10. enum class 与 switch

```cpp
void printRole(Role role) {
    switch (role) {
        case Role::System:
            std::cout << "system";
            break;
        case Role::User:
            std::cout << "user";
            break;
        case Role::Assistant:
            std::cout << "assistant";
            break;
        case Role::Tool:
            std::cout << "tool";
            break;
    }
}
```

枚举和 `switch` 很适合一起使用。

---

## 11. libagent 中为什么用 Role

代码位置：

```text
include/libagent/types.hpp
```

定义：

```cpp
enum class Role {
    System,
    User,
    Assistant,
    Tool
};
```

随后在：

```cpp
struct Message {
    Role role = Role::User;
    Content content;
    // ...
};
```

中作为消息角色类型。

相比直接使用：

```cpp
std::string role;
```

这种设计能保证内部消息角色只能是合法状态。

---

## 12. 为什么外部 API 仍然用字符串

很多 LLM API 最终需要：

```json
{
  "role": "user"
}
```

libagent 内部先使用：

```cpp
Role::User
```

再在协议边界转换为：

```text
"user"
```

因此设计上是：

```text
程序内部
enum class Role
      |
      v
serialization
      |
      v
外部 JSON / API 字符串
```

这种方式兼顾：

- 内部类型安全
- 外部协议兼容

---

## 13. 默认成员初始化

libagent 中：

```cpp
struct Message {
    Role role = Role::User;
};
```

这里：

```cpp
= Role::User
```

表示默认成员初始化。

因此：

```cpp
Message m;
```

如果没有显式指定 `role`，默认就是：

```cpp
Role::User
```

---

## 14. 面试常见问题

### enum 和 enum class 有什么区别？

可以回答：

> `enum class` 是 C++11 引入的 scoped、strongly typed enum。枚举值不会泄漏到外层作用域，使用时通常需要写成 `Role::User`；同时不会像传统 `enum` 那样轻易隐式转换为整数，因此类型安全性更高。

### 为什么现代 C++ 更常用 enum class？

因为它：

- 避免枚举值名字污染外层作用域
- 降低不同枚举类型之间误比较的风险
- 避免隐式整数转换
- 接口语义更清晰

---

## 15. 当前阶段只需要记住

```text
enum class Role { ... };
        |
        +--> 定义有限状态类型

Role::User
        |
        +--> 带作用域访问枚举值

enum class
        |
        +--> 比传统 enum 更强类型、更安全
```

在 libagent 中，`Role` 本质上就是：

> **用一个强类型有限状态集合表示聊天消息的四种角色。**
