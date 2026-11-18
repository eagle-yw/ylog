# sink 集合在创建期定型

`detail::LoggerBase` 的构造器过去无条件执行 `sinks_.append(StdoutSink{}, default_pattern)`，于是「新 logger 输出到哪」这条策略藏在共有面的构造器里，而调用方唯一的撤销手段是 `sinks()` 交出的可变 span：bench 写 `replace_default_sink`（替换 `front()`），测试写 `sinks()[initial].set_pattern(...)`，三个用例干脆劫持 `std::cout.rdbuf`。同一件事在三处被复述，且「结构何时可变」没有单一答案——同步组件随时可改，异步组件在首个入队后封板，靠 `configurable_` 加断言维持。

## 决策

- `Logger` 与 `AsyncLogger` 的构造器都不再选择目的地。sink 集合经 `LoggerConfig` / `AsyncLoggerConfig` 的 `sinks` 字段在创建期一次给定，用 `make_sinks(AnySink{...})` 拼装；`AnySink` 增加 `(sink, pattern)` 便捷构造，补回 `append_sink(sink, pattern)` 曾提供的杠杆。
- 删除 `append_sink` 的两个重载，以及 `configurable_` / `ensure_configurable()` / `seal_configuration()`。`SinkSet` 只保留以 `std::vector<AnySink>` 为入参的构造器，构造后只增删之外的一切照旧（逐 sink 的 `set_pattern` / `set_formatter`、扇出写、flush）。
- 「结构不可变」由类型保证：`AnySink` 删掉移动赋值，因此 `sinks()` 交出的 span 只能配置元素，不能替换或增删；`SinkSet` 不再有 `append`。
- 零 sink 是合法状态，语义是丢弃（与 `NullSink` 同义）。`Logger(LoggerConfig{.name = "x"})` 因此是一个安静的 logger，而不是编译错误。
- 「默认 logger 写 stdout」这条策略归 `Registry::default_logger()`：它是 default 槽位的**唯一**创建路径，初始集合就是 `make_sinks(AnySink{StdoutSink{}, default_pattern})`；`create("default")` 转调它，并对带非空 `sinks` 的配置报错。`ylog:logging` 的 `default_logger()` 只缓存这个引用。

## 为什么

- **局部性**：默认输出、默认槽位的创建、以及「谁来负责初始集合」集中在 `Registry::default_logger()` 一处；`LoggerBase` 只剩「存 name / level / sink 集合」。
- **杠杆**：`LoggerConfig` 是唯一的创建面，`Registry::create(LoggerConfig)` 与 `create_async` 只差一个类型；新增一种 sink 不触碰 logger 层。
- **测试面**：测试第一次可以只经公开接口决定输出目的地——`AnySink{OstreamSink{OstreamConfig{.stream = &oss}}}` 代替 `std::cout.rdbuf(oss.rdbuf())`，`sinks()[0]` 代替「默认 sink 占一位、新 sink 从 1 数起」的下标算术。
- **依赖方向**：`:logger.base` 不再 import `:sink.stdout`，ADR-0008 里「`:logger.base` 独立成分区是为了 `:async` 不必依赖 `:logger`」的动机不再被它自己的第 112 行抵消。

## Considered Options

- **只删构造器那一行，保留 `append_sink`**：改动最小，但 `sinks()` 仍是可变逃生门，`configurable_` 那套状态位也还在，「结构何时可变」依旧没有单一答案。
- **构造期注入 + 保留 `append_sink`**：两条给 sink 的路并存，调用方还得知道哪条在异步组件上会触发断言。
- **让 `LoggerConfig::sinks` 默认装一个 `StdoutSink`**：把隐式策略从构造器挪到结构体的默认成员，同一个问题换个位置，且 `Logger("x")` 依旧不可预测。
- **让 `Logger` 可移动，用 builder 的 `build()` 返回**：`build()` 返回非可移动对象本来就靠保证消除，引入 builder 只是多一层概念；`make_sinks` 已经足够。
- **把 `default_logger()` 留在 `:logging` 里懒创建**：构造期定型后它无法再给槽位追加 sink，两条创建路径的调用顺序会决定 default 槽位有没有输出，退回不可预测。
- **对保留名静默忽略 `config.sinks`**：调用方给了 sink 却拿不到，比报错更难查。

## Consequences

- 公开面破坏：`Logger("name")` 不再可用，`append_sink` 消失，`AsyncLoggerConfig` 新增 `sinks`（放在最后，让指定初始化保持既有书写顺序）。仓库内 20 个构造点与 19 个 `append_sink` 调用点全部迁移；`CONFIG`、`bench`、`example` 与 README 同步更新。0.x 没有 API 稳定承诺，ADR-0001 也不追求兼容层。
- `AsyncLoggerConfig` 因持有 `std::vector<AnySink>` 而不可拷贝：以左值传入时必须 `std::move`。
- `make_sinks` 的存在理由是 `AnySink` 不可拷贝，`std::vector<AnySink>` 无法用花括号初始化式构造；它只接 `AnySink`，不接受裸 sink，以免出现第二条构造路径。
- ADR-0008 的「配置期检查放在基座里」一条随之作废；ADR-0003 中「首个入队或 `stop()` 之后 `append_sink` / `set_pattern` 触发断言」的表述只剩 `set_pattern` 一侧，而且它现在是安全的（逐 sink 加 formatter 锁）。
- 基准失去「换个 sink 再测全局 logger」的能力：`bench_latency` 的 `null_sink_st (global logger)` 场景删除，只保留 `disabled-at-runtime (global logger)`。
