# 通用 sink 家族与 stdout 颜色

通用 sink 家族按「一个适配器一个 partition」补齐 `OstreamSink` / `CallbackSink` / `RingBufferSink`，`NullSink` 无状态无配置，落在 `ylog:sink` 内。`DistSink` 与 hourly sink 不采纳：`Logger` 的 `SinkSet` 本身就是扇出，而 `Sink` 只接收格式化后的行，组合 sink 无法为子 sink 各自格式化；hourly 与 `DailyFileSink` 同构，沿用 ADR-0005 的排除决定。

stdout 颜色不是「另一种 sink」，而是同一个 sink 的一种模式：`StdoutSink` / `OstreamSink` 持有 `ColorMode`（`Never` / `Automatic` / `Always`），上色动作落在 `AnySink`——颜色区间只有 formatter 知道（`PatternFormatter` 的 `%^`/`%$` 已产出 `color_range()`），而 sink 只拿到一行字符串。`Formatter` 基类新增 `color_range()` 虚函数（默认 `{0, 0}` 表示无区间），`AnySink::write` 在格式化之后、取输出锁之前按区间拼接 ANSI 转义，`Sink` concept 不变。sink 侧用可选协议 `color_mode()` 声明自己是否接受上色，与现有可选 `flush()` 用同一种 `if constexpr (requires { ... })` 探测，因此不引入第二套 sink 协议。

线程安全维持既有分工：具体 sink 实现无锁，`AnySink::output_mutex_` 是唯一串行点；唯一例外是 `RingBufferSink` 的读取句柄——`RingBufferStore` 自带互斥量，因为读取方在任意线程。`CallbackSink` 的回调在输出锁下调用，回调内禁止再向同一 logger 记录（重入不受支持），与 pattern flag、`err_handler` 的既有约定一致。

失败上报沿用 ADR-0005 的口径，不新增全局入口：配置期 `fail_config`（有异常时抛、`YLOG_NO_EXCEPTIONS` 下 assert + 终止），热路径 per-sink `err_handler`（`std::function<void(std::string_view)>`，默认限频写 stderr）。`FileSink` 目前热路径静默、且没有配置结构体，本次对齐为 `FileConfig` + `err_handler`；`OstreamSink` / `CallbackSink` 同样带 handler；`RingBufferSink` 无 I/O，不设 handler。

常用能力一并裁决：`flush_on(Level)` 采纳（`LoggerBase` 的配置期 setter，默认 `Off`），同步 logger 在写出后即时 flush，异步 logger 由工作线程在处理完该条之后 flush sink，不阻塞生产者，也不占队列槽位；公开 `should_log(Level) const` 采纳；`set_formatter(std::unique_ptr<Formatter>)` 采纳并扇出到每个 sink（逐 sink `clone()`，这正是 `Formatter::clone()` 的用途）；`sinks()` 访问器采纳，返回 `std::span<AnySink>`——结构不可变、元素可配置，后续 `append_sink` 会使之失效。`flush_every(interval)` 与全局 `set_error_handler` 排除。

## Considered Options

- **颜色放 sink 内**：需要 sink 接收 `Message` 或自行重解析 pattern，违背「Sink 是唯一接缝且只收字符串」的约束（同 ADR-0005 对 Message-aware 协议的否决）。
- **仿 spdlog 的 `stdout_color_sink` 独立类**：颜色是配置而非类型，按类型分裂会随 `Never`/`Automatic`/`Always` 与 sink 种类组合爆炸。
- **`DistSink` 作为组合 sink**：Ylog 的扇出在 `Logger`/`SinkSet`，且组合 sink 无法给子 sink 各自 formatter；需要多目的地就多 `append_sink` 一次。
- **全局 `set_error_handler`**：错误上报已按组件注入，全局可变状态会与 Registry 的显式所有权冲突；要统一默认值应改 `detail::errors` 的默认工厂。
- **编译期 level 裁剪（`SPDLOG_ACTIVE_LEVEL` 同款）**：宏必须与 BMI 一致，等于再复制一份 `YLOG_NO_EXCEPTIONS` 的双构建成本；热路径已有一次 relaxed atomic 读。
- **`ringbuffer` 用 `shared_ptr<sink>` 取回内容（spdlog 形状）**：会迫使 `AnySink` 暴露底层 sink 指针；改为「读句柄与被写的 sink 分离」，`RingBufferStore` 由调用方持有、`RingBufferSink` 持其指针。

## Known Divergences

- `CallbackSink` 回调收到的是格式化后的行（`std::string_view`），不是 spdlog 的 `log_msg`；级别、logger 名需由 pattern 自带。
- `RingBufferSink` 不提供 `last_formatted()` 方法于 sink 上，改由 `RingBufferStore` 读取；容量在 store 构造期固定。
- 颜色只覆盖 `%^`/`%$` 区间与 `%+` 已标记的区间；无区间不着色，与 spdlog color sink 一致，但 Ylog 不做 Windows 控制台 API 检测，`Automatic` 仅判 `isatty`。
- 不提供 `flush_every` / periodic worker、全局 error handler、`hourly_file_sink`、`dup_filter` / `dist` / `msvc` / `syslog` / `android` / `osx` sink。
