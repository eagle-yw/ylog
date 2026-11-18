# Sink 并发模型调研：传统实践与 C++26 新设施

调研问题：多个线程同时调用同一个 `Logger` 时，`Sink` 的串行化应由谁负责、用哪种模型？除「每 sink 一把锁」这类传统实践外，C++26 是否提供了新的可选方案？本文只记录事实与适配判断，不修改任何公开接口。

## 结论摘要

1. **日志的并发核心不是"生产者"而是"共享输出设备"**：无论哪种模型，最终都要让同一份 `std::ostream` / 文件描述符 / 终端按行串行落地。C++26 没有新增任何"并发安全 sink"抽象，新增的是内存回收（hazard pointer、RCU）与执行器编排（senders/receivers）。
2. **传统实践分两派**：spdlog 走「每 sink 一把 mutex」，把输出与配置变更放进同一把锁；Quill 走「每线程一个无锁 SPSC 队列 + 单后端线程」，把前端成本压到纳秒级，代价是参数二进制序列化与后端解码。
3. **`std::osyncstream`（C++20，非 C++26 新增）是被忽略的第三条路**：它的同步粒度是**底层 streambuf**，而不是某个对象。因此它天然解决「多个 Logger 各自持有 StdoutSink、却共享同一个 `std::cout`」的串扰问题；而每对象一把锁做不到这件事。
4. **C++26 的两块新拼图确实相关，但都不解决串行化**：`<hazard_pointer>`（P2530R3）与 `<rcu>`（P2545R4）解决"读多写少配置的安全回收"（`set_pattern` / `set_formatter` / `Registry::drop` 与并发读），读侧可做到零锁；`<execution>`（P2300R10）解决编排，不提供背压与队列。
5. **本地工具链尚未落地关键设施**：GCC 16.2.1 / libstdc++ 20260810 上 `<rcu>` 与 `<hazard_pointer>` **头文件不存在**，`<execution>` 只有 C++17 并行算法（`__cpp_lib_execution 201902L`），`std::execution::inline_scheduler` 不存在；`<syncstream>`、`std::function_ref`、`std::copyable_function`、`std::inplace_vector`、`<contracts>` 可用。Clang 23.1.5 使用同一份 libstdc++，结论相同。
6. **对 Ylog 的结论**：现阶段应选「每 `AnySink` 一把 mutex + 控制台用 `osyncstream` + 配置变更限配置期」，并把配置替换留成可替换 seam；hazard pointer / RCU 只能作为后续替换项，不能作为当前设计前提。
7. **与仓库规范的一处冲突**：`CODING_STANDARDS.md` 写「不使用 contracts、execution 等尚未稳定的特性」，但 contracts 已由 P2900R14 进入 C++26 且本地 GCC 16 已可编译 `contract_assert`。这句应改成"暂不采用 + 理由"，而不是"尚未稳定"。

## 一手来源与做法

### 1. spdlog 1.17.0：每 sink 一把 mutex，输出与配置同锁

`base_sink` 以 `Mutex` 为模板参数，持有 `Mutex mutex_`（`include/spdlog/sinks/base_sink.h:18,39`），类注释原文：

> "base sink templated over a mutex (either dummy or real) … locking is taken care of in this class - no locking needed by the implementers."（`base_sink.h:6-9`）

`log()` 在调用 `sink_it_()` 前加锁，`flush()`、`set_pattern()`、`set_formatter()` 同样加锁（`base_sink-inl.h:26-48`）：

```cpp
template <typename Mutex>
void base_sink<Mutex>::log(const details::log_msg &msg) {
    std::lock_guard<Mutex> lock(mutex_);
    sink_it_(msg);
}
```

要点：**输出与配置共用一把锁**，所以 `set_pattern` 不可能与正在进行的格式化交错；换 dummy mutex 即可整体关闭加锁。代价是热点 sink（终端、慢盘）的锁驻留时间等于格式化 + I/O 时间，多线程尾延迟直接叠加。

### 2. Quill：每线程 SPSC 队列 + 单后端线程

README 原文（`github.com/odygrd/quill`，master）：

> "Each frontend thread owns a **lock-free SPSC queue**. `LOG_*` macros binary-serialize arguments directly into that queue — no shared state, no contention between threads, no formatting work on the caller."

> "A **single backend worker** drains all queues, merges events in timestamp order, invokes the per-argument-pack decode function … runs `{fmt}` formatting and the `PatternFormatter`, and writes the resulting log lines or metric samples to the attached `Sink`s."

队列可配置：

> "**Configurable Queue Modes**: `bounded/unbounded` and `blocking/dropping` options with monitoring on dropped messages, queue reallocations, and blocked hot threads."

README 给出的前端延迟（v13.0.0 / RHEL 9.4 / i5-12600 / GCC 14.2，50th 百分位）：1 线程数值型日志 6 ns，4 线程 8 ns；同表 spdlog 为 271 ns / 557 ns。测量方式是"20 次调用取平均、间隔约 2 ms"。

要点：**前端零锁，串行化被推迟到唯一的后端线程**；代价是热路径必须序列化参数（不能直接持有 `std::string_view`）、需要 decode 函数指针、跨线程时间戳合并，以及"后端线程是单点"——后端卡住时靠队列策略丢弃或阻塞，而不是靠锁排队。

### 3. POSIX `O_APPEND`：免锁文件写入的边界

POSIX.1-2024 `write()` 对 `O_APPEND` 的规范原文：

> "If the O_APPEND flag of the file status flags is set, the file offset shall be set to the end of the file prior to each write and no intervening file modification operation shall occur between changing the file offset and the write operation."

但 RATIONALE 明确划了边界：

> "This volume of POSIX.1-2024 does not specify the behavior of concurrent writes to a regular file from multiple threads, except that each write is atomic … Applications should use some form of concurrency control."

管道/终端另有 `{PIPE_BUF}` 限制：小于等于 `PIPE_BUF` 的写不会交错，超过则可能在任意边界交错。

要点：`O_APPEND` 只保证「定位 + 写」这一步不被插入，**不保证应用层的行完整性**。因此免锁文件 sink 只有在「一行一次 `write()`、且行长受控、目标为普通文件」时才近似成立；Ylog 的 `std::ofstream` 走缓冲层，天然不满足这个前提。

### 4. `std::osyncstream`（C++20）：同步粒度是底层 streambuf

cppreference `std::basic_osyncstream` 页面（`__cpp_lib_syncbuf = 201803L`，C++20）：

- 写入先落在内部 `basic_syncbuf`，**只有 `emit()` 或析构时才提交**到最终目标 streambuf；`std::endl` 只是"登记" flush 意图。
- 保证范围原文：对"同一个最终目标缓冲区"的输出无数据竞争、不交错；**前提是所有写者都经由某个 `osyncstream`**，绕过它直接写原流则保证失效。

要点：这是标准库唯一一个"按共享目标同步"的设施。它不解决文件 sink 的双实例问题（两个 `ofstream` 是两个 streambuf），但对 `std::cout` / `std::cerr` 这类进程级共享目标是精确匹配。代价是每行一次缓冲分配与一次内部加锁，且不能替代 `fsync` 语义。

### 5. C++26 `<hazard_pointer>`：读侧零锁的安全回收

cppreference `Standard library header <hazard_pointer> (C++26)`（P2530R3）列出：

- `hazard_pointer`："single-writer multi-reader pointer that can be owned by at most one thread at any point of time"；成员 `empty()`、`protect(const atomic<T*>&)`、`try_protect(T*&, const atomic<T*>&)`、`reset_protection(...)`、`swap()`，除析构外均 `noexcept`；不可拷贝、可移动。
- `hazard_pointer_obj_base<T, D>::retire(D d = D()) noexcept`：对象自退役，延迟回收。
- `make_hazard_pointer()` 构造，另有 `std::swap` 特化。

要点：写者用原子指针换新配置，读者 `protect()` 拿到指针后即可无锁读取，旧对象 `retire()` 后由实现延迟释放。**这正是 `set_pattern` / `set_formatter` / `Registry::drop` 需要的语义**：读侧不增加引用计数 RMW，写侧不必等所有读者退出。

### 6. C++26 `<rcu>`：更便宜的读侧，更重的写侧

cppreference `Standard library header <rcu> (C++26)`（P2545R4）：

- `rcu_domain`：`lock()` / `try_lock()` / `unlock()`，均 `noexcept`；不可拷贝。
- `rcu_default_domain()` 返回静态域；`rcu_synchronize(dom)` "blocks until a protection region unlocks"；`rcu_barrier(dom)` 可能执行已调度操作并阻塞到全部完成；`rcu_retire(p, d, dom)` 调度释放，"potentially allocating memory"。
- `rcu_obj_base<T, D>::retire()` 为 `noexcept`。

要点：读者进入保护区的成本比 hazard pointer 更低（通常是一次 thread-local 计数），但**写者必须承担同步等待**（`rcu_synchronize` 阻塞）。对日志这种"配置极少变、写入极频繁"的场景，读侧收益明显；对"运行期频繁 set_pattern"的场景不划算。

### 7. C++26 `<execution>`：编排，不是队列

cppreference `C++26` 页面列出的相关条目：`Execution control library`（P2300R10 等）、`Parallel scheduler`（P2079R10、P3804R2）、`Coroutine task types`（P3552R3 等）、`A utility for creating execution environments`（P3325R5）。

要点：senders/receivers 能表达"调度 → 格式化 → `bulk` 扇出 → `when_all` 收尾"，也带来了 C++26 的 `parallel_scheduler` 与 `task`。但它**不提供队列容量/溢出/背压策略**——这些仍需自建。用它替换 ADR-0003 的"每组件一个 jthread"，等于把一条已定决策重开成执行器选型，收益是结构化组合，代价是编译期与抽象成本。

### 8. C++26 零散工具（非并发专用，但影响 sink 实现）

| 设施 | 论文 | 与 sink 的关系 |
|---|---|---|
| `std::function_ref` | P0792R14、P3961R1 | `err_handler` 改引用语义，热路径零分配；但不能持有状态所有权 |
| `std::copyable_function` | P2548R6 | 替代 `std::function`，可要求 `noexcept` / const 可调用，错误处理路径更可控 |
| `std::inplace_vector` | P0843R14、P3981R2、P4022R0 | 固定容量队列/批缓冲，省掉 `deque` 的块分配 |
| contracts | P2900R14、P3819R0、P3886R0 | 可表达"sink 只在 owner 线程调用"这类前置条件；但违规处理策略与现有错误模型冲突 |

## 模型对比

前提：所有模型都假设"一行日志 = 一次完整的输出单元"。

| 模型 | 热路径成本 | 顺序保证 | 同目标多实例 | 轮转/保留语义 | 实现复杂度 | 主要失败模式 |
|---|---|---|---|---|---|---|
| A 每 sink 一把 mutex（spdlog） | 无竞争约一次原子操作；竞争时叠加最慢 sink 的 I/O 时间 | 每 sink 内 FIFO | 不覆盖 | 天然兼容（轮转在锁内） | 低 | 慢盘/终端拖尾延迟；`err_handler` 若重入日志会自锁 |
| B 每线程 SPSC + 单后端（Quill） | 纳秒级，无锁 | 后端可按时间戳跨线程合并 | 覆盖（后端单点扇出） | 需后端串行执行轮转 | 高（参数序列化 + decode） | 后端单点停顿 → 丢消息/阻塞；`fork()` 不友好 |
| C 每 sink owner 线程（actor） | 前端只入队 | 每 sink 内 FIFO | 覆盖 | 兼容 | 中高（每 sink 一线程 + 关闭顺序） | 线程数随 sink 增长；`flush` 需要屏障 |
| D 有界 MPSC + 单消费者 | 前端一次入队 | 全局 FIFO（单消费者） | 覆盖 | 兼容 | 中 | 队列满 → block/overrun/discard；生命周期需 `jthread` 排空 |
| E 按线程/哈希分片 | 竞争最低 | 跨分片无序 | 覆盖，但同文件多 writer | 不兼容（轮转需全局状态） | 中 | 顺序丢失；多 writer 打开同一文件会互相截断 |
| F `std::osyncstream` | 每行一次缓冲 + 内部锁 | 目标 streambuf 内按块原子 | 覆盖（按 streambuf） | 不涉及（需与文件 sink 配合） | 低 | 绕过 osyncstream 的写者破坏保证；每行分配 |
| G 不可变配置 + 原子交换 | 读侧零锁 | 不涉及 | 不涉及 | 不涉及 | 中 | 旧对象回收时机；写侧可能阻塞（RCU） |

补充三点跨模型的判断：

1. **A 与 F 不是替代关系**：A 保证"sink 对象"的状态安全（`buffer_`、`cached_tm_`、`current_size_`），F 保证"目标 streambuf"的字节不交错。两者都在时，F 才是多余的那一层；任一缺失，另一层补不上。
2. **B 与 D 的差别在"前端是否序列化"**：D 仍然把 `Message` 以对象形式入队（Ylog 的 `Message` 持有 `std::string`），前端成本是构造 + 拷贝；B 把参数压成字节流，前端只剩 memcpy。前者保持了 `Message` 的类型安全与 `Sink` 接缝，后者换来了纳秒级延迟但要求重写 `Logger` 热路径。
3. **E 与文件 sink 结构性冲突**：轮转/按日依赖"当前文件大小、下次日界"这类全局状态，分片后要么共享状态（回到锁），要么各写各的文件（破坏命名约定）。

## C++26 是否带来新方案

结论：**C++26 没有为 sink 的串行化提供新机制，但为"配置热替换"提供了两个真正的升级项**。

- 真正新增且相关：`<hazard_pointer>`、`<rcu>`。它们把"读多写少的配置指针"从"`shared_ptr` 原子操作 + 引用计数争用"或"全局锁"里解放出来，读侧可完全无锁。这直接对应 Ylog 的三个真实痛点：`PatternFormatter::set_pattern` 与 `format` 并发、`AnySink::set_formatter` 与 `write` 并发、`Registry::drop` 与已发出的 `Logger&` 生命周期。
- 名义新增但不解决核心问题：`<execution>`（P2300R10）。它提供调度与组合，提供的 `parallel_scheduler` 是通用池，仍然要自己决定队列容量、溢出策略与 `flush` 屏障——也就是 D 模型里那些决策一个都没省掉。
- 并非新增但常被忽略：`std::osyncstream`（C++20）。在"多个 Logger 共享 stdout"这个 Ylog 必然出现的场景里，它比"每个 StdoutSink 各持一把锁"更正确。
- 新增且当下就能用的小工具：`std::function_ref`、`std::copyable_function`、`std::inplace_vector`、`contracts`。

### 本地可用性实测

环境：GCC 16.2.1 20260810（`__GLIBCXX__ 20260810`）、Clang 23.1.5（使用同一份 libstdc++）。命令：`g++ -std=c++26 -fsyntax-only <probe>.cpp`。

| 设施 | GCC 16.2.1 | Clang 23.1.5 | 证据 |
|---|---|---|---|
| `<rcu>` | 不可用 | 不可用 | `fatal error: rcu: No such file or directory` |
| `<hazard_pointer>` | 不可用 | 不可用 | `fatal error: hazard_pointer: No such file or directory` |
| `<execution>` senders | 不可用 | — | 仅有 `__cpp_lib_execution 201902L`；`std::execution::inline_scheduler` 不是 `std::execution` 成员 |
| `<syncstream>` / `osyncstream` | 可用 | 可用（同 libstdc++） | 编译通过 |
| `std::function_ref` | 可用 | 可用 | `__cpp_lib_function_ref 202603L` |
| `std::copyable_function` | 可用 | 可用 | `__cpp_lib_copyable_function 202306L` |
| `std::inplace_vector` | 可用 | 可用 | `__cpp_lib_inplace_vector 202603L` |
| `<contracts>` / `contract_assert` | 可用 | — | 探针编译通过 |

含义：**hazard pointer 与 RCU 目前只能作为设计储备，不能作为实现前提**。把接口留成"可替换的配置指针 seam"是当下唯一可行的路径；真正切换要等 libstdc++ 落地这两个头文件。

## 对 Ylog 的适配判断

### 现有接缝上的共享可变状态

| 位置 | 可变状态 | 并发后果 |
|---|---|---|
| `src/sinks/sink.cppm:70` | `AnySink::buffer_` | 两个线程同时 `write` 会互相覆盖格式化缓冲，输出错乱 |
| `src/formatter.cppm:391-397` | `formatters_`、`custom_flags_`、`cached_tm_`、`last_log_secs_` | `set_pattern` 重编译与 `format` 遍历并发 → 读到悬垂 `unique_ptr`，不只是错乱 |
| `src/sinks/sink_file_rotating.cppm:195-196` | `stream_`、`current_size_` | 并发写导致大小计数漂移、轮转时机错误、同一文件交错 |
| `src/sinks/sink_file_daily.cppm:219-223` | `stream_`、`next_rotation_`、`created_files_` | 同上，且保留策略可能删掉正在写的文件 |
| `src/logger.cppm:97` | `sinks_`（`std::vector<AnySink>`） | 运行期 `append_sink` 会重分配，迭代器失效 |
| `src/registry.cppm:68` | `loggers_` | `drop()` 后已发出的 `Logger&` 悬垂 |

`Logger` 用 `std::atomic<Level>`（`src/logger.cppm:96`）暗示了多线程可用，但上述状态全部无保护——这是当前最大的契约缺口。

### Ylog 特有的所有权问题

spdlog 的 sink 由 `shared_ptr` 共享，所以"每 sink 一把锁"同时覆盖了所有使用该 sink 的 logger。Ylog 的 `AnySink` 是值语义：`Model<T>` 内持有 `T sink_`（`src/sinks/sink.cppm:56-66`），`Logger` 持有 `std::vector<AnySink>`（`src/logger.cppm:97`）。因此：

- 两个 `Logger` 各自 `append_sink(FileSink{"app.log"})` → 两个独立的 `ofstream`、两把锁、同一个文件 → 每 sink 加锁**不能**防止互相截断与交错。
- 两个 `Logger` 各自持有默认 `StdoutSink` → 共享 `std::cout` 的 streambuf → 每 sink 加锁**不能**防止行内交错。

这不是锁选型问题，是所有权粒度问题。可选解：文档声明"同一目标只允许一个 sink 实例"、在 `Registry` 层做路径唯一校验、或对控制台改用 `osyncstream`。引入 `shared_ptr<sink>` 会改变 `AnySink` 的值语义，并牵连 ADR-0005 的 per-sink `err_handler`，不建议为此重开。

### 建议（分层，最小改动优先）

1. **同步路径：`AnySink` 内持一把 `std::mutex`**，覆盖 `write` / `flush` / `set_pattern` / `set_formatter`，与 spdlog 的 `base_sink` 同构。`Sink` concept 不变，`Logger` 不变，符合"`Sink` 是唯一 sink 接缝"的约束。锁的持有范围要短：只包 `formatter_->format` + `model_->write`，不包 `Logger` 的扇出循环（避免一把 logger 锁串起所有 sink 的尾延迟）。
2. **控制台 sink 改用 `std::osyncstream` 成员 + 显式 `emit()`**，让同步粒度落在 `std::cout` / `std::cerr` 的 streambuf，覆盖跨 logger 的场景。这是标准库层保证，比自定义锁正确。
3. **配置变更明确为配置期 API**：ADR-0004 已规定 `add_flag` "注册限配置期"，把 `Logger::set_pattern` / `AnySink::set_formatter` 一并写进同一条约束，就可以合法地不为其加锁；运行期热更新作为单独特性另立 ADR。
4. **异步路径无需额外加锁**：ADR-0003 已保证每组件独占一个工作线程，sink 访问天然串行；此时 `AnySink` 的锁只会在"同步 Logger 与异步 Logger 共享同一 sink"时起作用，而 Ylog 的 `AnySink` 值语义本来就不共享。
5. **`err_handler` 规定为不可重入**：锁内回调，若 handler 再写日志会自锁/递归。这条要写进 ADR-0005 的错误契约。
6. **为未来留 seam**：把"The formatter 指针 + pattern 字符串"收进一个 `shared_ptr<const Config>`，读侧 `atomic_load` / `load(memory_order_acquire)`。今天用 `std::atomic<std::shared_ptr<T>>`（C++20，引用计数有 RMW 成本）；等到 libstdc++ 提供 `<hazard_pointer>`，把同一个 seam 换成 `protect()` 即可，公开接口不动。
7. **可选 `NullMutex` 策略**：spdlog 用模板参数允许关锁。Ylog 可以用一个编译期策略（或 `YLOG_SINGLE_THREADED` 开关）给单线程用户去掉原子操作，符合"提供编译期开关"的既有规范。

### 不建议的选项

- **每 sink 一个 owner 线程（C）**：与 ADR-0003 的"每组件一个工作线程"重复，线程数随 sink 增长，且关闭顺序需要额外屏障。
- **按线程分片（E）**：与轮转/按日的全局状态冲突，跨分片顺序丢失，收益不足以抵消语义破坏。
- **每线程 SPSC + 参数序列化（B）**：等于重写 `Logger` 热路径（`std::format_string` 编译期检查、`Message` 的对象语义都要放弃），超出"核心能力移植"的范围；若将来要做，应作为独立 ADR。
- **`<execution>` 重写异步**：会重开 ADR-0003，且引入一个标准库尚未实现的依赖。

### 与既有规范的冲突

- `CODING_STANDARDS.md:22` 写「不使用 contracts、execution 等尚未稳定的特性」。事实上 contracts 已由 P2900R14 进入 C++26，且本地 GCC 16 可编译 `contract_assert`；`<execution>` 的 sends/receivers 也已标准化，只是 libstdc++ 未实现。建议把这句话改成"暂不采用，理由：错误模型走异常 + `err_handler`，contract 违规处理策略未定；执行器选型会重开 ADR-0003"，避免用"尚未稳定"描述已定标准。
- ADR-0003 的"异步不侵入 `Logger`"与本文建议 1 不冲突：锁在 `AnySink` 内部，`Logger` 的接口与扇出逻辑不变，异步路径也不会经过它两次。

## 参考链接清单

- spdlog `base_sink.h`：`tmp/spdlog-1.17.0/include/spdlog/sinks/base_sink.h`
- spdlog `base_sink-inl.h`：`tmp/spdlog-1.17.0/include/spdlog/sinks/base_sink-inl.h`
- Quill README：<https://github.com/odygrd/quill>
- POSIX.1-2024 `write()`：<https://pubs.opengroup.org/onlinepubs/9799919799/functions/write.html>
- `std::basic_osyncstream`：<https://en.cppreference.com/w/cpp/io/basic_osyncstream>
- `<hazard_pointer>`（C++26）：<https://en.cppreference.com/w/cpp/header/hazard_pointer>
- `<rcu>`（C++26）：<https://en.cppreference.com/w/cpp/header/rcu>
- C++26 特性总表：<https://en.cppreference.com/w/cpp/26>
- P2530R3 Hazard Pointers：<https://open-std.org/JTC1/SC22/WG21/docs/papers/2023/p2530r3.pdf>
- P2545R4 Read-Copy Update：<https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2545r4.pdf>
- P2300R10 `std::execution`：<https://open-std.org/JTC1/SC22/WG21/docs/papers/2024/p2300r10.html>
- P2900R14 Contracts：<https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p2900r14.pdf>
- P0792R14 `function_ref`：<https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p0792r14.html>
- P2548R6 `copyable_function`：<https://open-std.org/JTC1/SC22/WG21/docs/papers/2023/p2548r6.pdf>
- P0843R14 `inplace_vector`：<https://open-std.org/JTC1/SC22/WG21/docs/papers/2024/p0843r14.html>
