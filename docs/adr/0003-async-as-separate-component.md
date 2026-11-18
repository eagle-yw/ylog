# 异步作为独立组件

异步日志不实现 `Sink` concept，也不塞进 `Logger` 内部：由独立的 `AsyncLogger` 组件把 `Message` 入队，每个组件独占一个工作线程投递给 sink，`Logger` 保持不变；Registry 以最小工厂/查找接口管理它，现有 `set_level` / `flush_all` / `drop_all` 覆盖异步组件。这样异步边界上仍保留 Message 的 Level / SourceLocation 与每个 sink 的 Formatter，不会因为 `Sink` 接缝只收 `std::string_view` 而丢失元数据。代价是每组件一个线程、Registry 职责小幅扩展，以及多一个公开接缝。

## 追加：flush 用「序号 + 条件变量」，队列里只有消息

最初实现把 flush 做成一条入队的控制消息（`FlushCommand` + `promise/future`）。评审时发现两个后果：控制消息不受容量约束，让「队列有界」只对消息成立；停止后 worker 已经退出，`flush()` 改由调用线程直接 flush sink，多条并发 flush 会同时进入 sink 集合。

现在回到 spec 描述的机制：消息入队时分配递增序号，`flush()` 记下当前最大序号作为目标；worker 每写完一条更新已写序号，追平目标后 flush sink、把「已落盘序号」推到目标并唤醒等待者。

- 队列里只有消息，长度严格受 `queue_capacity` 约束，不随 flush 调用次数增长。
- 并发 flush 各等各的目标，不会被提前结清。这也是「多条并发 flush 合并到同一条控制消息」被否掉的原因：合并的命令可能排在某调用方自己的消息之前，屏障会在它的消息落盘前返回。
- 控制消息消失后，丢弃/覆盖逻辑不必再区分消息与控制消息，`drop_oldest` 退化为删队首。

停止后的 `flush()` 仍由调用线程执行 sink flush：单 sink 的串行由 `AnySink::output_mutex_` 提供，但「sink 只被 worker 访问」这条隐性前提在停止后不再成立。若将来出现依赖线程约束的 sink（例如按线程缓存状态的实现），这条路径需要重新引入串行化。

## Considered Options

- 实现 `Sink` concept 的队列装饰器：改动最小，但 `AnySink` 在调用线程已完成格式化，Message 元数据在到达异步层前被消费，下游无法按级别或源位置决策。
- `Logger` 内部可选队列：热路径增加分支，并把调度塞进日志入口，违背"不修改 Logger"的接缝约束。
- 全局共享线程池：与 spdlog 行为最贴近，但会把 Registry 从"按名称管理 Logger"撑成调度资源管理器，并引入 shutdown 顺序问题；每组件独占线程把生命周期局部化。
