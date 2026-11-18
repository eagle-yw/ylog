# 文件轮转与按日 sink 语义

Ylog 以两个独立 `Sink` 采纳 spdlog 的文件轮转能力：`RotatingFileSink` 处理大小阈值，`DailyFileSink` 处理本地零点日界；现有 `FileSink` 保持纯文件输出，按小时 sink 不在范围。两个 sink 只接收格式化后的字符串行，`Sink` concept 不变。按日边界由写入时的 `system_clock::now()` 判定，因此与 spdlog 使用 `msg.time` 的语义存在微小偏差；不为 `AnySink` 增加 Message-aware 可选协议，以保留唯一接缝。

配置期（构造、校验、初始打开、递归创建父目录）抛带可操作信息的异常；热路径（`write` / 轮转 / `flush`）失败交给构造期注入的 per-sink `err_handler`（`std::function<void(std::string_view)>`，默认写 stderr 且约每秒一条），不抛异常；`YLOG_NO_EXCEPTIONS` 下配置失败以 `assert` + 终止处理。

大小轮转要求 `max_size > 0`、`max_files <= 200000`；严格 `new_size > max_size` 触发，轮转后写入本条消息；活动文件用原名，备份用 `{basename}.{i}{ext}`，保留最近 `max_files` 个，`max_files == 0` 表示无备份；`rotate_on_open` 默认关闭。按日轮转以本地 `00:00:00` 为边界，`>=` 下次边界时先打开新文件再写入本条；文件名固定 `{basename}_{YYYY-MM-DD}{ext}`，不开放 strftime 样式；`max_files` 只清理本进程运行期间创建的最近 N 个文件，0 表示不删除。两个新 sink 默认追加已有文件，提供 `truncate_on_open` 显式截断；不逐条 flush，仅显式 `flush()` 与大小轮转前 flush。

公开接口由 `ylog:sink.file.rotating` 与 `ylog:sink.file.daily` 两个 partition 提供，各导出一个配置结构体和一个 sink 类，构造只接受按值配置结构体。

## Considered Options

- 单一“万能文件 sink”：把大小轮转、按日轮转与纯输出塞进一个类，构造与热路径都需要按模式分支，违背深模块原则。
- 扩展 `Sink` concept 或在 `AnySink` 增加 Message-aware 可选 `write`：能消除时间语义偏差，但引入第二套 sink 协议，破坏“Sink 是唯一 sink 接缝”的约束；收益不足以抵消接缝扩散。
- 完全逐条复刻 spdlog：包括 hourly、自定义日格式、fsync、历史运行文件清理与重命名重试；超出 map 的能力边界，并把平台分支带进公开接口。

## Known Divergences

- 按日边界使用写入时系统时间，而不是 `Message` 的时间戳。
- 不提供 hourly sink、strftime 样式日文件名、fsync、历史运行文件清理。

## 追加：文件生命周期收进私有接缝

`DailyFileSink` 与 `RotatingFileSink` 曾各自持有一份文件名拆分、父目录创建、流打开与热路径写/刷失败上报，两份只差上报前缀。这些现在集中在实现分区 `ylog:sink.file.lifecycle`（`module ylog:sink.file.lifecycle;`，不导出任何名字，同 `:async.queue` / `:logger.base`），两个 sink 经 `import :sink.file.lifecycle;` 使用：

- 路径操作：`split_stem_and_extension`、`path_exists`、`file_size_or_zero`；
- 流生命周期：`detail::file_stream`——配置期 `open`（失败交给各自的 `fail_*_config`，与校验失败共用类型前缀）、热路径 `reopen` / `write` / `flush`（失败上报并清流状态）、`close`，把「打开、写、刷、关、上报、清状态」收成一处；
- 热路径上报文案的唯一出口 `report_io_failure`：流失败与 sink 自己的文件系统失败（删旧日文件、删最旧备份、改备份名）共用同一形状，sink 名作为参数传入。

边界不变：轮转触发条件、文件命名（`_{YYYY-MM-DD}` 与 `.{i}`）、保留策略仍留在各自 sink。`err_handler` 按次取自 `config_.err_handler`，不能在构造期抓成员地址——两个 sink 都按值移动（`AnySink` 构造即如此），抓地址在移动后悬垂。`FileSink` 不创建父目录、有自己的 failure latch，不参与本次合并；公开接口零变化。

合并保留了合并前的一条既有偏差：父目录建不出来时，配置期错误仍报成 `failed to open '<文件路径>'`，路径指文件而不是父目录。这不在本次集中化的范围内，改动它会变更公开的错误文案。
