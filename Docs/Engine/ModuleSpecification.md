---
date: 2026-04-22
category:
  - C++
tag:
  - 开发规范
  - 模块
---

# 模块规范

## 依赖原则

1. 禁止循环依赖
2. 层级调用：下层模块不能包含上层模块

## 单例与线程安全规范

1. **禁止内部自调用**：模块内部实现中，严禁调用自身的静态获取实例方法

## Pimpl 惯习与头文件隔离

在头文件（`.h`）中，尽量不要 `#include` 具体的类实现，而是使用**前向声明**。

```cpp
// Logger.h
class Config; // ✅ 前向声明：告诉编译器有个叫 Config 的类，但不知道它长啥样

class Logger {
    Config* m_config; // ✅ 指针/引用可以用前向声明
    // Config m_config; // ❌ 错误：编译器需要知道 Config 的大小
};
```

在 `.cpp` 文件中再 `#include "Config.h"`。

## RAII 与作用域最小化

### 规范 1：严禁手动 lock/unlock

永远使用 `std::lock_guard` 或 `std::unique_lock`。它们会在作用域结束时自动释放锁，防止因异常或 return 导致的死锁。

### 规范 2：锁的范围最小化

```cpp
void Logger::Log(const std::string& msg) {
    {
        std::lock_guard lock(m_mutex);
        m_buffer.push_back(msg); // ✅ 只锁住内存操作，极快
    } // 锁在这里自动释放
    WriteToFileAsync(); // ✅ 在锁外进行耗时 IO
}
```

## 智能指针所有权规范

| 场景 | 指针类型 | 说明 |
|------|----------|------|
| 独占所有权 | `std::unique_ptr<T>` | 默认选择，资源拥有者负责销毁 |
| 共享所有权 | `std::shared_ptr<T>` | 仅在多个模块共用资源时使用 |
| 观察指针 | `T*` 或 `T&` | 不负责生命周期，只是使用 |

## 初始化顺序规范

### 构造即初始化

不要在构造函数里做复杂的逻辑（特别是虚函数调用或依赖全局单例）。

### 两阶段初始化

- **构造函数**：只初始化成员变量，设置默认值。不加锁，不抛异常。
- **Init() 方法**：做真正的启动工作（读取文件、创建线程）。允许抛异常。

### Meyer's Singleton（迈耶单例）

这是 C++11 后最安全的单例实现方式，线程安全且无死锁风险。

```cpp
Logger& Logger::GetInstance() {
    static Logger instance; // ✅ C++11 保证这行代码是线程安全的，只执行一次
    return instance;
}
```

## 异常与断言规范

| 模式 | 使用方式 | 适用场景 |
|------|----------|----------|
| Debug | `assert` 或自定义宏 | 检查逻辑错误（如 `index >= size`） |
| Release | `return false` 或 `throw` | 运行时错误（如文件找不到） |

> **注意**：`assert` 在 Release 模式会被编译器优化掉，不适合用于运行时错误检查。
