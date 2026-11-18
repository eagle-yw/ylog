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

**Formatter**：将 `Message` 按模式转换为输出文本的规则。
_Avoid_: 格式器、layout、pattern engine

**FieldFormat**：由聚合类型字段生成的格式串，供 Formatter 展开 Message。
_Avoid_: 字段模板、format_info

**PatternFlag**：调用方注册的单个 pattern flag 处理器，实现 `PatternFlag` concept，把 Message 与共享的 `std::tm` 写入输出文本。
_Avoid_: 自定义 flag、flag handler、add_flag

**Logger**：调用方记录日志时面对的模块；它持有级别与模式，并把记录交给 sink。
_Avoid_: 记录器实例、log object

**AsyncLogger**：把 Message 入队、由工作线程投递给 Sink 的 Logger。
_Avoid_: 异步记录器、async sink

**Registry**：按名称管理 `Logger` 实例的集中位置，负责创建、查找与移除。
_Avoid_: 日志管理器、factory
