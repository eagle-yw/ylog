# 自定义 pattern flag 接缝

Ylog 开放 flag 粒度的扩展，但注册点只在 `ylog:formatter` 的 `PatternFormatter` 实例上：调用方通过 `add_flag(char, handler)` 注册满足 `PatternFlag` concept 的处理器。`PatternFlag` 要求 `std::copy_constructible<T>` 与 `format(const Message&, const std::tm&, std::string&) -> void`；公开接口不导出虚基类、`detail` 类型、构造参数转发、factory 重载或 `clone()` 要求。编译 pattern 时每个 `%` 出现都从注册原型复制独立 handler；padding 由 `PatternFormatter` 统一施加，未注册的 flag 仍按字面输出。自定义 flag 覆盖同字符内置 flag；重复注册在配置期抛 `std::invalid_argument`。`Formatter` 增加虚 `set_pattern(std::string_view)`，`AnySink::set_pattern` 只委托给当前 formatter 以保留实例级注册；`Logger` 增加接受 `unique_ptr<Formatter>` 的 `append_sink` 重载，用于注入配置好的 formatter。注册限配置期，热路径无锁；`clone()` 深拷贝注册表。

## Considered Options

- 注册点放在 `AnySink`：sink 会承担 formatter 的职责，违背“Formatter 负责 pattern”的模块边界，并使 `set_formatter` 注入的自定义 formatter 难以共存。
- 全局或 Registry 级静态注册表：clone 实现简单，但引入进程级可变状态与锁，并把 Registry 从按名称管理 Logger 撑成格式化资源管理器。
- 复制 spdlog 的 `add_flag<T, Args...>` 与 `custom_flag_formatter`：与 ADR-0001 的不兼容决策冲突，且会把虚接口与实现类型推到公开接口；ADR-0002 也禁止反射信息出现在导出签名里。

## 与既有 ADR 的关系

ADR-0003 中“`Logger` 保持不变”限定于异步路径：异步不侵入 Logger。本决策只增加一个 formatter 注入重载，不引入队列、线程或调度，不重开异步接缝。

## 追加：formatter 注入点随 ADR-0011 迁移

上文「`Logger` 增加接受 `unique_ptr<Formatter>` 的 `append_sink` 重载」已随 ADR-0011 消失：sink 集合改为创建期定型后，注入配置好的 formatter 写成 `AnySink{sink, std::unique_ptr<Formatter>}`，再由 `make_sinks` 装进 `LoggerConfig::sinks`。`AnySink::set_pattern` 的委托语义与「formatter 归属 sink」不变。
