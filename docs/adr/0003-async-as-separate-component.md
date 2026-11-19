# 异步作为独立组件

异步日志不实现 `Sink` concept，也不塞进 `Logger` 内部：由独立的 `AsyncLogger` 组件把 `Message` 入队，每个组件独占一个工作线程投递给 sink，`Logger` 保持不变；Registry 以最小工厂/查找接口管理它，现有 `set_level` / `flush_all` / `drop_all` 覆盖异步组件。这样异步边界上仍保留 Message 的 Level / SourceLocation 与每个 sink 的 Formatter，不会因为 `Sink` 接缝只收 `std::string_view` 而丢失元数据。代价是每组件一个线程、Registry 职责小幅扩展，以及多一个公开接缝。

## 追加：flush 用「序号 + 条件变量」，队列里只有消息

最初实现把 flush 做成一条入队的控制消息（`FlushCommand` + `promise/future`）。评审时发现两个后果：控制消息不受容量约束，让「队列有界」只对消息成立；停止后 worker 已经退出，`flush()` 改由调用线程直接 flush sink，多条并发 flush 会同时进入 sink 集合。

现在回到 spec 描述的机制：消息入队时分配递增序号，`flush()` 记下当前最大序号作为目标；worker 每写完一条更新已写序号，追平目标后 flush sink、把「已落盘序号」推到目标并唤醒等待者。

- flush 请求按目标序号结算：目标之内的消息写出后立刻落盘，不再等到队列排空。`flush_on(Level)` 因此可以复用同一个机制——入队成功时把目标序号推到该条，生产者不等待、不占队列槽位，`flush()` 的屏障语义也不变。
- 队列里只有消息，长度严格受 `queue_capacity` 约束，不随 flush 调用次数增长。
- 并发 flush 各等各的目标，不会被提前结清。这也是「多条并发 flush 合并到同一条控制消息」被否掉的原因：合并的命令可能排在某调用方自己的消息之前，屏障会在它的消息落盘前返回。
- 控制消息消失后，丢弃/覆盖逻辑不必再区分消息与控制消息，`drop_oldest` 退化为删队首。

停止后的 `flush()` 仍由调用线程执行 sink flush：单 sink 的串行由 `AnySink::output_mutex_` 提供，但「sink 只被 worker 访问」这条隐性前提在停止后不再成立。若将来出现依赖线程约束的 sink（例如按线程缓存状态的实现），这条路径需要重新引入串行化。

## 追加：ErrorHandler 的触发面与上报粒度

spec 只承认三类触发点：① 非阻塞策略的每次丢弃/覆盖；② 停止后仍尝试入队；③ 某个 sink 投递失败。三处边界按下面口径解释。

- **③ 包含 flush 失败。** 「投递失败」按结果定义：没写进 sink、或写进了但没落盘，都是没送出去。`flush_sinks()` 的失败因此归入 ③，不新增第四类。
- **② 每次都报，不做「只报一次」去重。** User Story 8 是「every drop/overrun reported, so that loss is never silent」；早先的 `report_stopped_once` 让停止后的第二条起彻底静默。输出量交给默认 handler 的每秒限频控制，而不是在组件里静默丢弃通知。
- **③ 的「每 sink 一次」按「连续失败」实现。** 一个 sink 处在失败状态时只上报一次，它下一次写成功即复位，再次失败会重新上报。作用域是组件生命周期内的一段失败区间，不是整个进程；这样持续故障不会按消息条数刷屏，恢复后复发仍然可见。flush 失败不参与这次去重——它只由显式 flush 触发，频率由调用方控制。

## 追加：err_handler 的重入边界与 sink 配置期

handler 总是在某个线程上同步调用：丢弃/停止上报发生在调用线程（锁外），sink 写入与 flush 失败发生在工作线程。由此得到两条调用方契约。

- **工作线程上的 handler 不得重入本组件的 `log()` / `flush()` / `stop()`。** Block 策略下队列满时 `log()` 要等 worker 腾出空位，而 worker 正卡在 handler 里；`flush()` 要等 worker 推进序号，等的是同一个自己。两条都是自死锁。默认 handler 只写 stderr，不会触发；自定义 handler 若需要重入，得自己转投到别的队列或线程。spec 只规定了「handler 抛出的异常被吞掉」，重入不在支持范围内，也不打算用可重入锁去兼容它。
- **`append_sink` / `set_pattern` 只在配置期合法。** worker 在构造末尾就已启动，之后改 sink 集合会与它的遍历竞争：`SinkSet` 的槽位既没有锁，也不能容忍中途扩容。首个入队与 `stop()` 之后，两个入口触发 `assert`；断言只抓得住单线程误用（先 log 再 append），真正的并发修改仍是数据竞争，接口不为此加锁——加锁只能让竞争变得安静，不能消除「worker 正持着迭代器、调用方 push_back」这个窗口。若将来需要运行期热插拔 sink，正确方向是让 worker 独占 sink 集合、把配置变更作为队列命令投递给 worker。

## Considered Options

- 实现 `Sink` concept 的队列装饰器：改动最小，但 `AnySink` 在调用线程已完成格式化，Message 元数据在到达异步层前被消费，下游无法按级别或源位置决策。
- `Logger` 内部可选队列：热路径增加分支，并把调度塞进日志入口，违背"不修改 Logger"的接缝约束。
- 全局共享线程池：与 spdlog 行为最贴近，但会把 Registry 从"按名称管理 Logger"撑成调度资源管理器，并引入 shutdown 顺序问题；每组件独占线程把生命周期局部化。

## 追加：sink 集合改为创建期定型（ADR-0011）

上面「`append_sink` / `set_pattern` 只在配置期合法」一条只对 `set_pattern` 仍然成立，且它现在是安全的：formatter 归属 sink，`set_pattern` 逐 sink 取 `formatter_mutex_`，与 worker 的遍历并存。

`append_sink` 已随 ADR-0011 删除：sink 集合在构造期由 `AsyncLoggerConfig::sinks` 一次给定，worker 启动之后不存在「被追加」的可能，`configurable_` / `ensure_configurable()` / `seal_configuration()` 一并消失。因此原文末句「若将来需要运行期热插拔 sink」不必再解：能力本身不再提供。

## 追加：队列与 Flush 屏障归一个内部 owner

上面的机制一度分散在 `AsyncLogger` 的六处入口（`stop` / `flush` / `log` / `enqueue` / `run` / 驱逐），改一处 `flush_on` 要同时追准入、驱逐、进度、唤醒与排空顺序。现在这些转换归 `src/async_queue.cppm` 的 `detail::QueueBarrier`：它是 `:async.queue` 实现分区且不导出任何名字——同一模块的 `:async` 能用，模块外看不见也命名不了（同 `:logger.base`），公开面仍然只有 `AsyncLogger`。

- **三条转换规则集中在一处**：准入（Block 等空位、OverrunOldest 覆盖队首、DiscardNew 丢弃，以及 `flush_on` 把屏障目标推到刚入队的那条）、屏障（`arm_barrier` 判定这次等什么，`next_work` 负责结算）、停机（`begin_stop` 固定排空集合，排空走完 `next_work` 才给 `finished`）。
- **锁边界不变**：接口的每个出口只交出「锁外要做的事」——待投递的消息、待结算的落盘目标、丢失条目的文本。Sink 投递与 `err_handler` 调用仍然一律在队列锁外（见上面「err_handler 的重入边界」）。
- **丢失条目的文本改由队列拷出**。原实现交给上报的是指向 `Message::formatted()` 的视图，只在「该分支恰好没有 move」时才成立；现在消息进队列那一刻就在锁内拷好，入队成功路径不额外分配。
- 对外行为不变：屏障语义、队列长度只受 `queue_capacity` 约束、重复 `stop()`、停止后由调用线程 flush sink 都保持原样；新增两个用例锁定「覆盖 × 屏障 × 停机」同时发生。

## 追加：工作线程批量出队

原实现 `next_work` 一次只摘一条消息：每条消息两次队列锁（出队、回报进度），外加一次容量条件变量唤醒。队列满时生产者每条都要 park/wake 一次，Block 策略因此出现性能悬崖——4 线程 250k 条实测只有 44.6k 条/秒，比不阻塞的 `OverrunOldest` 低两个数量级。

现在 `next_work` 一次摘一批（上限 `QueueBarrier::max_batch = 64`），消息搬进调用方提供的缓冲，整批写完才 `note_delivered(最后一条序号)` 推进进度；空出的槽位用一次 `notify_all` 叫醒全部等待的生产者。三条约束保持不变：

- **屏障仍按序号结算，且这一批不越过待结算目标**：批量被 `flush_target_ - processed_sequence_` 截断，`flush_on` 仍紧跟在触发那一条之后落盘，不会被整批推迟。
- **队长约束不变**：`queue_.size() <= capacity_`；同一时刻最多有一批（≤ 64 条）在 worker 手里，所以进程内的消息总数上限是 `capacity + max_batch`，而 `queue_capacity` 的含义仍然只约束队列本身。
- **锁边界不变**：批在锁内搬进缓冲，投递与落盘在锁外；上一批的 `Message` 也在锁外销毁。

实测（250k 条、队列 8192、pattern `%v`，`bench_async`）：4 线程 44.6k → 1.6M 条/秒，单线程 1.1M → 2.6M。

剩下的退化是单锁队列的固有形状：所有生产者在同一把 mutex 上串行，线程越多争用越重（8 线程 637k、10 线程 531k）。再往上需要换队列结构（有界 MPMC 环或多队列），不在本次范围；`queue_capacity` 调大能直接减少阻塞次数，这是调用方现有的手段。
