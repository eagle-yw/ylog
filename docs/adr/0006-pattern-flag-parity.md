# pattern flag 全集与 spdlog 语义对齐

`ylog:formatter` 的 `PatternFormatter` 支持 spdlog 1.17.0 的 flag 全集：`%+`、`%n`、`%l`、`%L`、`%v`、`%t`、`%P`、`%a`/`%A`、`%b`/`%h`/`%B`、`%c`、`%C`/`%Y`、`%D`/`%x`、`%m`/`%d`、`%H`/`%I`、`%M`/`%S`、`%e`/`%f`/`%F`、`%E`、`%p`、`%r`、`%R`、`%T`/`%X`、`%z`、`%^`/`%$`、`%@`/`%g`/`%s`/`%#`/`%!`、`%%`、`%u`/`%i`/`%o`/`%O`、`%&`；未注册的 flag 原样输出 `%` 加该字符，末尾单个 `%` 被丢弃。语义逐条取自 research 分支的 `research/pattern-flags.md`（53 条事实，一手来源 spdlog 1.17.0），并由 `test/pattern_oracle_test.cpp` 直接以 spdlog 1.17.0 的输出为 oracle 覆盖。

padding 与 spdlog 的 `scoped_padder` 同构：默认补在值左侧（值右对齐），`-` 补在右侧，`=` 两侧各半；宽度上限 64；宽度后紧跟 `!` 时从尾部截断到宽度。补位字符只有空格，`%08l` 是宽 8 而非零填充。固定字段宽度（`%c` 24、`%E` 10 等）参与宽度计算而不是按实际输出长度计算，`%+` 与未知 flag、`%^`/`%$` 不受 padding 影响。

`%t` 需要的线程 id 在 `Message` 构造点捕获（Linux 取内核 TID，与 spdlog 的 `os::thread_id()` 一致），因此异步 sink 报告的是生产者线程；`%P` 在每次格式化时读取 PID；`%z` 用 `tm_gmtoff` 计算本地偏移。`%^`/`%$` 只记录位置、不写转义序列；由于 `Message` 不可变，区间状态落在 `PatternFormatter` 上并通过 `color_range()` 暴露，与 spdlog 把它写进 `log_msg` 的可观察结果一致。

标准库没有这三项的可用数值接口（`std::thread::id` 不可取数且哈希不保证不碰撞、没有进程设施、`std::tm` 没有偏移字段；C++20 tzdb 在 MSVC 缺失且依赖运行时 tzdata），因此平台相关代码只放在 `ylog_detail_os`：`thread_id()`、`process_id()`、`utc_offset_minutes()`。`message`/`formatter` 里不出现平台宏，移植时只改这一个模块。

## Considered Options

- 只补 `%t`、其余 flag 保持未知：改动最小，但 `%+`、`%@`、`%c`、`%z` 等常用 pattern 会退化为字面输出，等于把 spdlog 的既有用户挡在门外。
- 为每个 flag 手写期望字符串：测试会变成“实现的自证”；同一批断言无法发现两个实现同时偏离 spdlog 的事实。
- 把 color range 放进 `Message`：需要可变的共享状态，破坏 `Message` 在格式化与输出之间只读传递的约束。
- 同时实现 MDC 容器：`%&` 才有内容可输出，但 MDC 需要线程局部容器与公开读写接口，超出“补齐 pattern”的范围。

## Known Divergences

- `%l` 与 `%+` 里的 level 名沿用 `ylog:level` 的名称表（`Info`），spdlog 默认是 `info`；`%L` 短名与 spdlog 一致。
- `%&` 被识别但恒为空：Ylog 尚无 MDC 容器，等价于 spdlog 在 MDC 为空时的输出。
- `%y`（两位年）是 Ylog 保留的额外 flag，spdlog 没有。
- 只有 localtime 模式，没有 `pattern_time_type::utc`；`%z` 恒为本地偏移。
- spdlog 的未知 flag + 截断兼容分支（`%3!q` 输出函数名前缀）不复刻：`%3!q` 输出 `%q`，`%3!!` 走正常的 `%!` + 截断。
- `%+` 不做逐秒日期缓存，只影响性能，不影响输出。
