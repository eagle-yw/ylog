# 异步作为独立组件

异步日志不实现 `Sink` concept，也不塞进 `Logger` 内部：由独立的 `AsyncLogger` 组件把 `Message` 入队，每个组件独占一个工作线程投递给 sink，`Logger` 保持不变；Registry 以最小工厂/查找接口管理它，现有 `set_level` / `flush_all` / `drop_all` 覆盖异步组件。这样异步边界上仍保留 Message 的 Level / SourceLocation 与每个 sink 的 Formatter，不会因为 `Sink` 接缝只收 `std::string_view` 而丢失元数据。代价是每组件一个线程、Registry 职责小幅扩展，以及多一个公开接缝。

## Considered Options

- 实现 `Sink` concept 的队列装饰器：改动最小，但 `AnySink` 在调用线程已完成格式化，Message 元数据在到达异步层前被消费，下游无法按级别或源位置决策。
- `Logger` 内部可选队列：热路径增加分支，并把调度塞进日志入口，违背"不修改 Logger"的接缝约束。
- 全局共享线程池：与 spdlog 行为最贴近，但会把 Registry 从"按名称管理 Logger"撑成调度资源管理器，并引入 shutdown 顺序问题；每组件独占线程把生命周期局部化。
