# Registry 的命名空间与所有权

同步 `Logger` 与异步 `AsyncLogger` 共用一个名字空间：一个名字只属于一个组件，`Registry` 内部用一张表存 `{synchronous, asynchronous}` 两个指针（恰有一个非空），不再维护两张平行 map。`create(name)` 对同名同步 logger 幂等，撞上异步组件时抛 `std::invalid_argument`；`create_async(name, config)` 撞上任何已用名字都抛 `std::invalid_argument`；`get`/`get_async` 按类型查找，名字属于另一类时返回 `nullptr`——查找不跨类型，创建不共享名字。`contains(name)` 两侧都算。

所有权只有一份：`Registry` 是唯一持有者，`drop`/`drop_all` 先把条目移出表、再在锁外析构，因此异步组件的排空发生在 `drop` 返回之前，而不是等某个外部持有者离开作用域。异步组件对外只给引用，不发布 `shared_ptr`，所以「最后持有者析构才停机」这种模型在这个接口下无法成立，规范按现状写。

被 spec 的方法清单遗漏、但实际保留的公开面：`Registry::contains`、`Registry::set_level(name, level)`，以及 `AsyncLogger::stop`、`queue_capacity`、`overflow_policy`。前两个让「按名字操作」与「按名字查询」对称；`stop` 是显式停机的唯一入口（析构只是复用它）；两个只读访问器让测试与调用方不必猜配置。

## Considered Options

- **两张平行 map**：同步/异步各一张，查找时天然不混淆。代价是 `create`/`create_async`/`get`/`get_async`/`contains`/`drop`/`set_level`/`flush_all` 里各写一遍双分支，加第三种 logger 要改六处。
- **`shared_ptr` 发布所有权，drop 只摘除不停机**：贴合 spec 原文「最后持有者析构时停机」。但 `create`/`create_async` 现在返回引用，改成 `shared_ptr` 会改掉调用方所有写法；而对一个进程级注册表来说，外部持有者本来就无法判断「最后一个」何时离开，这条语义在实践中不会更准确。
- **让 `create` 在撞上异步名时返回 `Logger&` 的某种代理**：没有诚实的返回值可选，只能抛。
