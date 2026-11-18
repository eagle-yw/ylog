# default logger 归 Registry 持有，自由函数移入独立分区

`default_logger()` 过去是 `ylog:logger` 内的函数内 static，与 `Registry` 里名为 `default` 的槽位是两个不同实例：`create("default")` 会造出同名不同物。本次裁决为单一身份——默认 logger 就是 `Registry` 中名为 `default` 的常驻槽位，`default_logger()` 返回它，因此 `ylog::set_level()` 与 `Registry::set_level("default", …)` 是同一件事。

分区结构上，`ylog:registry` 依赖 `ylog:logger`（Registry 持有 Logger），把 `default_logger()` 直接写进 `:logger` 会造成分区级循环依赖。`default_logger()` 与八个自由函数（`set_level` / `set_pattern` / `log` / `trace` / `debug` / `info` / `warning` / `error` / `critical`）一并移入新的 `ylog:logging`，它同时 import `:logger` 与 `:registry`；`ylog:logger` 只留 `Logger` 类本身。分区名不能叫 `default`——它是关键字，而模块分区名只接受标识符，写成 `export module ylog:default;` 会被扫描器误当成主接口单元。对外仍是 `import ylog` 一个入口，只有直接 `import ylog:logger` 的调用方会丢掉这些符号。

`default` 是常驻名：`drop("default")` 返回 `false`，`drop_all()` 跳过它。理由是 `default_logger()` 缓存一个 `static Logger&` 引用（实例归 Registry，引用只解析一次，热路径仍然无锁），摘除会让该引用悬垂。语义上与 `std::cout` 同类：进程级默认输出不接受关闭。代价是 `drop_all()` 不再是「清空一切」。

槽位懒创建：首次 `default_logger()` 或 `create("default")` 时建。因此 `contains("default")` 在任何人使用默认 logger 之前为 `false`，如实反映「有没有人用过」。`create("default")` 天然幂等（`create` 对同名同步 logger 本就幂等），不需要为默认名写特例。`create_async("default")` 抛 `std::invalid_argument`——`default` 是保留名，与槽位是否已经创建无关（见下方追加）。

不提供 `set_default_logger`。spdlog 有它，是因为它用 `shared_ptr` 发布所有权，换指针不涉及销毁；ADR-0007 已否决该方案，`create` / `create_async` 只返回引用，Registry 是唯一持有者。setter 需要调用方外部持有实例，与此所有权模型不能并存。要换目标就显式持有并使用那个 logger；默认槽位只服务自由函数。

## Considered Options

- **保留独立 static + 补 `set_default_logger`**：两套语义永久共存，用户第一次 `create("default")` 就踩到同名不同物；且 setter 同样与 ADR-0007 冲突。
- **`default_logger()` 每次经 Registry 查表**：`drop("default")` 的语义最干净（下次调用重建），但每条日志多一次无竞争互斥——而 `ylog::info()` 正是最随手的那条路。
- **允许 `drop("default")` 且保留缓存引用**：悬垂引用，直接排除。
- **`set_default_logger(std::string_view name)`**：把「默认」做成指向已注册名字的间接层，可行，但缓存引用退化成每次查表，回到上一条的锁开销。
- **饿创建槽位**（Registry 构造时即建）：`contains("default")` 恒真，但「空注册表」这个概念随之消失。

## Consequences

- 新增 `ylog:logging` 分区，`ylog.cppm` 要与其余分区一并 `export import`。
- `drop_all()` 的注释与测试须写明「default 除外」，`drop("default")` 的返回值语义要有测试覆盖。
- 直接 `import ylog:logger` 的调用方会丢掉默认 logger 与自由函数，须在 CHANGELOG / 迁移说明里点出。当前 `test/` 与 `example/` 均只 `import ylog`，无实际影响。

## 追加：default 槽位的初始 sink 集合（ADR-0011）

sink 集合改为创建期定型之后，`default_logger()` 无法再「懒创建之后再追加一个 `StdoutSink`」。因此：

- `Registry::default_logger()` 成为 default 槽位的**唯一**创建路径，构造时就把 `StdoutSink` 放进初始集合；`create("default")` 转调它，并对带非空 `sinks` 的配置报错。
- 上文「`create("default")` 天然幂等，不需要为默认名写特例」仍然成立的是身份（同一个实例），不再成立的是「`create` 路径对默认名毫无特例」：`create(LoggerConfig)` 现在要先判保留名再转调。
- 「槽位懒创建」保留，懒创建的判定点从两个入口收成一个。

## 追加：保留名先于懒创建生效

早先的实现在 `create_async` 里只判「名字是否已被占用」，而 default 槽位是懒创建的，于是新建 Registry 上 `create_async("default", …)` 会成功占坑：此后 `default_logger()` 抛「已被异步组件占用」，`drop("default")` 恒 `false`、`drop_all()` 又跳过这个名字，这个异步组件再也摘不掉、也排不空。占位结果取决于调用顺序，说明保留名此前只是散落在创建与摘除路径上的几处 `name == "default"` 比较，而不是一条不变量。

现在保留名的三条规则由 `Registry` 的私有类型 `DefaultSlot` 独家持有，`create` / `create_async` / `drop` / `drop_all` 都经它判定：

- **保留在查表之前生效**：`create_async("default", …)` 无条件抛 `std::invalid_argument`，失败不留下槽位（`contains("default")` 仍为 `false`）；
- **懒创建不变**：首次 `default_logger()` / `create("default")` 才构造，初始集合恰好一个 `StdoutSink`；
- **常驻不变**：`drop("default")` 为 `false`，`drop_all()` 跳过它。常驻判定与保留名同源，不另写一份比较。

副产品：保留名只接受空配置——`create(LoggerConfig)` 对非空 `sinks` **或**非 `Info` 的 `level` 报错，级别只经 `set_level` 修改。这与 ADR-0011 拒绝「静默忽略外部指定的 sinks」是同一个理由。
