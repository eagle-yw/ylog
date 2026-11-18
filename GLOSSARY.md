# Ylog

Ylog 是一个基于 C++26 模块构建的日志库。它把日志级别、消息、格式化与输出目的地组织成可组合的模块，供调用方通过稳定的接口记录日志。

## 语言

**Level**：日志的严重性等级，如 trace、debug、info、warning、error、critical。
_Avoid_: 严重度、日志等级

**Message**：一次日志记录的内容与其来源位置的结合体，是格式化与输出之间的传递单位。
_Avoid_: 日志条目、LogEntry、record

**SourceLocation**：一条日志产生处的源文件、行号与函数信息。
_Avoid_: 调用点、调用位置

**Sink**：日志条目的输出目的地；通过 `Sink` concept 约束，决定一条已格式化的消息送往何处。
_Avoid_: 输出器、handler、destination

**Sink Set**：一个 logger 在创建期定型的 sink 集合，由 `LoggerConfig` / `AsyncLoggerConfig` 的 `sinks` 字段给出，用 `make_sinks(AnySink{...})` 拼装。创建后不可增删，只能逐 sink 改 pattern；空集合是合法状态，语义是丢弃。结构不可变由类型保证（`AnySink` 不可移动赋值）。
_Avoid_: sink 列表、输出列表、sink 注册表

**ColorMode**：Sink 对颜色的态度。`Never` 永不着色，`Always` 始终着色，`Automatic` 在配置期按输出是否连着终端定型为前两者之一。上色由 `AnySink` 执行，Sink 只用可选的 `color_mode()` 声明模式。
_Avoid_: 彩色输出、color scheme

**NullSink**：丢弃所有行的 Sink，无配置、无状态；用于基准测试，或临时关掉某个目的地。它是 Sink 协议可选部分（如 `flush()`）的验证点。
_Avoid_: 空 sink、noop sink

**OstreamSink**：把行写进调用方给定的任意 `std::ostream` 的 Sink。不拥有 stream，生命周期由调用方保证；颜色按 `ColorMode` 配置，非 `std::cout` / `std::cerr` 的输出一律按非终端定型。
_Avoid_: stream sink、stderr sink

**CallbackSink**：把格式化后的整行交给调用方函数的 Sink。级别与 logger 名需由 pattern 自带；回调在输出锁下调用，因此不得重入同一个 logger。
_Avoid_: handler sink、lambda sink

**RingBufferStore**：环形缓冲的读句柄，与被写入的 Sink 分离，由调用方持有。容量在构造期固定，读取方可在任意线程。它只存行，不参与格式化，也不属于 Sink。
_Avoid_: ring buffer、日志缓存

**RingBufferSink**：把行写进调用方持有的 `RingBufferStore` 的 Sink。store 必须比使用它的 logger 活得久；它无 I/O，因此没有 `err_handler`。
_Avoid_: memory sink、ringbuffer

**Formatter**：将 `Message` 按模式转换为输出文本的规则。
_Avoid_: 格式器、layout、pattern engine

**FieldFormat**：由聚合类型字段生成的格式串，供 Formatter 展开 Message。
_Avoid_: 字段模板、format_info

**PatternFlag**：调用方注册的单个 pattern flag 处理器，实现 `PatternFlag` concept，把 Message 与共享的 `std::tm` 写入输出文本。
_Avoid_: 自定义 flag、flag handler、add_flag

**Logger**：调用方记录日志时面对的模块；它持有级别、模式与创建期定型的 Sink Set，并把记录交给 sink。
_Avoid_: 记录器实例、log object

**AsyncLogger**：把 Message 入队、由工作线程投递给 Sink 的 Logger。
_Avoid_: 异步记录器、async sink

**OverflowPolicy**：`AsyncLogger` 队列满时的处置方式，构造后不可变。`Block` 等待空位，`OverrunOldest` 覆盖最旧的一条，`DiscardNew` 丢弃新来的一条。
_Avoid_: 背压策略、丢弃模式

**ErrorHandler**：热路径失败的通知回调，形如 `std::function<void(std::string_view)>`；既是 `AsyncLogger` 的配置，也是各 Sink 配置结构体的 `err_handler`。入参是借用视图，只在调用期间有效。默认实现按 sink 各持一份约 1 秒的限频窗口。
_Avoid_: 错误回调、日志回调

**Flush 屏障**：`flush()` 的返回条件——它返回时，此前入队的消息都已写出、且每个 Sink 都已落盘。
_Avoid_: 同步点、flush 完成

**Flush 阈值**：`flush_on(Level)` 设定的级别，达到该级别的消息写出后由组件主动落盘，默认 `Off`（从不触发）；同步组件在调用线程 flush，异步组件交给工作线程，生产者不等落盘。
_Avoid_: 自动 flush、flush 间隔

**丢弃上报 / 覆盖上报**：非阻塞策略每丢掉一条消息就经 ErrorHandler 通知一次；组件不提供丢弃计数器。
_Avoid_: 丢弃统计、溢出次数

**Registry**：按名称管理 `Logger` 实例的集中位置，负责创建、查找与移除；创建是 sink 集合的定型点。
_Avoid_: 日志管理器、factory

**Default 槽位**：Registry 中名为 `default` 的常驻条目，由 `Registry::default_logger()` 独家创建，初始 Sink Set 就是一个 `StdoutSink`；不可摘除，因为自由函数缓存了它的引用。`default` 是**保留名**：它先于槽位创建生效，`create_async("default")` 无条件拒绝且不留槽位，而 `contains("default")` 在首次使用前仍为 `false`。
_Avoid_: 全局 logger、根 logger、默认实例
