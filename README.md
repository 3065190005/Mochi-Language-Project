# 🍡 Mochi 语言

**动态、异步、可嵌入的解释型脚本语言**

Mochi 是一款现代动态编程语言，拥有简洁的类与继承、原生并发 Actor 模型、丰富的内置数据结构、运算符重载、深拷贝、自动析构以及强大的 FFI。它被设计为可嵌入 C++ 宿主程序，同时提供友好的 REPL 交互环境、图形界面库和全面的标准库支持。

---

## ✨ 核心特性

- **类与单继承** – `class` / `super` / `this`，支持动态方法与运算符重载
- **变量声明** – 使用 `loc` 定义局部变量
- **数据类型** – `nil`, `bool`, `number`, `string`, `array`, `dict`, `function`, `class`, `instance`
- **控制流** – `if` / `while` / `for` / `for-in` / `break` / `continue`
- **函数与闭包** – `fn` 关键字，一等公民，环境捕获
- **迭代器协议** – `for-in` 支持内置类型、`range` 和自定义 `iter()`
- **错误处理** – `throw` / `try-catch` / `error()`
- **模块系统** – `import("name")` 内置 / `"file.mochi"` 脚本 / `"lib.mochimod"` 动态库
- **并发 Actor 模型** – `spawn` / `send` / `ask` / `reply` / `Future`，线程安全
- **运算符重载** – `+`, `-`, `*`, `/`, `%`, `[]` 等，可自定义
- **深拷贝与自动析构** – `clone()`、`destroy()` 自动管理资源
- **FFI** – 双向调用 C++ 与 Mochi 对象
- **GUI 系统** – 基于 SDL2 的扁平化界面，线程安全，搭积木风格
- **标准库** – `fs`, `math`, `json`, `random`, `string`, `array`, `dict`, `utf`, `dir`, `socket`, `pack`, `dialog`, `gui` 等
- **REPL** – 交互式环境，支持多行与状态保持

---

## 🚀 快速开始

```mochi
# 运行脚本文件
./mochi my_script.mochi

# 启动 REPL
./mochi

**Mochi 代码示例：**

```mochi
loc x = 10;
loc y = 20;
print(x + y);

class Animal {
    init(name) { this.name = name; }
    speak() { print(this.name + " makes a sound."); }
}

class Dog : Animal {
    speak() { print(this.name + " barks!"); }
}

loc d = Dog("Buddy");
d.speak();

## 📚 文档

- [语言参考手册](document.html)（完整语法、内置函数、标准库一览）
- [标准库详细说明](mochi_std_library.html)（每个库的函数、参数与示例）
- [GUI编程指南](mochi_gui.html)（每个库的函数、参数与示例）
- [解释器源码解析](sources.html)（核心架构与实现细节）

---

## 🛠️ 构建与依赖

Mochi 使用 **C++17** 编写，依赖库：

- **SDL2 ver2.32.8**（GUI 支持，可选）
- **SDL2_ttf ver2.24.0**（字体渲染）
- **SDL2_gfx**（圆角等图形，已在源码中）
- **tinyfiledialogs**（系统对话框，已在源码中）

编译时需要链接对应库，SDL2具体依赖请看mochi_gui.html

---

## 📦 第三方模块

任何人都可以用 C++ 编写模块并编译为 `.mochimod` 动态库，通过 `import()` 加载。只需实现 `mochi_module_init` 函数并返回字典，即可为 Mochi 扩展任意功能。

---

## 🌟 特别感谢

欢迎一起培养mochi。

---

*&copy; 2026 Mochi Language Project*