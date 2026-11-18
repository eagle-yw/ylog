# 共享日志基座放在实现分区

`Logger` 与 `AsyncLogger` 的公共面（`name` / `level` / `set_level` / 两个 `append_sink` / `set_pattern` / `set_formatter` / `sinks` / `should_log` / `flush_on` + `flush_level`）连同存储完全一致，只有写入路径不同。把 `SinkSet` 与六个级别便捷方法抽走之后，剩下的成员仍在两个组件里逐行各写一份：冗余从「实现」缩小到「签名加存储」，但依旧要同步维护两处。

## 决策

- `detail::LoggerBase<Derived>`（`src/logger_base.cppm`）持有 `name_` / `level_` / `flush_level_` / `sinks_`，提供上述成员、六个级别便捷方法与配置期检查，通过 CRTP 把 `trace..critical` 转回派生类的 `log()`。
- `Logger` 与 `AsyncLogger` 公开继承它，各自只留差异：同步直写，或入队加工作线程与序号屏障。
- `flush_on` 的阈值与 setter 在基座里，触发动作留给派生类：同步组件在写出后直接 `flush()`，异步组件把请求折算成一次序号屏障请求（见 ADR-0003）。阈值与 `level_` 同属运行期可改的原子配置，不参与 `ensure_configurable`。
- 基座独占**实现分区** `ylog:logger.base`（`module ylog:logger.base;`，不写 `export`）。`ylog` 的主接口 unit 不导出它，`:async` 因此只 import 这个分区，不必为拿一个 CRTP 基类而 import `:logger`。

## 为什么不是组合

组合（`detail::LoggerCore core_` 成员加逐成员转发）把重复从「实现」换成「签名加一行转发」：两个组件的公开面本来就必须一模一样，转发层是净增代码。级别便捷方法无论如何要继承——公有继承一次解决存储、成员与 CRTP 两件事。

## 为什么不是私有继承加 using 声明

私有继承下基类成员在派生类里不可见，必须为 8 个普通成员加 6 个模板成员逐个写 `using`，既重复又容易漏；#35 的实施期间就漏过一次，`info()` 一度在公开面消失。

## 为什么不拆成独立的 interface partition

`export module ylog:logger.base;` 会被 GCC 16 直接拒绝（`interface partition is not exported`），唯一修法是在主接口 `export import :logger.base;`——那等于把 `detail::LoggerBase` 名字交给调用方，与 `CODING_STANDARDS.md`「`detail` 内的接缝不进入公开接口」冲突。实现分区既满足「`:async` 不依赖 `:logger`」，又保持基类「可达但不可命名」：调用方能用 `Logger` 的全部成员，却写不出 `detail::LoggerBase<Logger>` 这个名字。

## 代价

- `detail` 进入公开基类层次：布局与成员可见性由基类决定，`detail` 的改动会波及 `Logger` / `AsyncLogger` 的布局。库没有 ABI 承诺，接受。
- 「实现分区可被同模块其他分区 import」是 P1779 之后的规则；GCC 16 接受。若将来换到只允许主接口 import 实现分区的工具链，回退方案是把基座放回 `:logger`、让 `:async` import `:logger`——公开面不变，只是内部多一条编译期依赖。
- 配置期检查（`seal_configuration` / `ensure_configurable`）也放在基座里：同步组件从不解封，异步组件在首个入队与 `stop()` 时封板，见 ADR-0003 的重入与配置期条目。

## 追加：公共面与配置期检查随 ADR-0011 收缩

ADR-0011 之后，公共面里的两个 `append_sink` 被删除，换成 `LoggerConfig` / `AsyncLoggerConfig` 的 `sinks` 字段；sink 集合在创建期定型，基座不再需要 `configurable_` / `ensure_configurable()` / `seal_configuration()`，上文「配置期检查也放在基座里」一条作废。`sinks()` 仍然交出 `std::span<AnySink>`，但 `AnySink` 已删掉移动赋值，所以这个视图只能配置元素，不能替换或增删。

同一次改动还去掉了 `:logger.base` 对 `:sink.stdout` 的依赖（基座构造器不再装默认 stdout sink）：本 ADR 让基座独占实现分区的动机，从此不再被基座自身的反向依赖抵消。
