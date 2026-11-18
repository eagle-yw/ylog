# spdlog 1.17.0 文件轮转与按日 sink 行为清单

研究工单：[spdlog 文件轮转与按日 sink 行为清单](https://github.com/eagle-yw/ylog/issues/4)。一手来源为 `tmp/spdlog-1.17.0/include/spdlog/`，每条事实附源码行号；只记录事实，不含设计建议。

路径缩写（前缀 `/home/eagle/workspace/Playground/cpp/Ylog/tmp/spdlog-1.17.0/include/spdlog/`）：`RS`/`RSI`=sinks/rotating_file_sink.h/-inl.h，`DS`=sinks/daily_file_sink.h，`HS`=sinks/hourly_file_sink.h，`BS`/`BSI`=sinks/basic_file_sink.h/-inl.h，`FH`/`FHI`=details/file_helper.h/-inl.h，`OS`/`OSI`=details/os.h/-inl.h，`C`/`CI`=common.h/-inl.h，`LH`/`LHI`=logger.h/-inl.h。

## 1. 大小触发轮转

1. 构造拒绝 `max_size == 0`（抛异常），`max_files` 上限 200000（RS:23；RSI:36-42）。
2. 构造打开 `calc_filename(base, 0)` 并用 `file_helper_.size()` 初始化 `current_size_`；`rotate_on_open && current_size_ > 0` 时立即轮转并置 0（RSI:43-48）。
3. `calc_filename(filename, 0)` 返回原名；`index > 0` 时分离主名/扩展名生成 `{basename}.{index}{ext}`（RSI:51-64）。
4. 扩展名按最后一个 `.` 拆分；无点、开头点、末尾点、目录分隔符中的点不视为扩展名（FH:34-47；FHI:130-148）。
5. 序号语义注释为 `log.txt → log.1.txt → log.2.txt …`，最后一个备份被删除（RS:42-46）。
6. 写入前先格式化得 `formatted.size()`，估算 `current_size_ + formatted.size()`（RSI:108-112）。
7. 轮转判断为严格 `new_size > max_size_`；估算超限时先 `flush()`，仅当真实 `size() > 0` 才 `rotate_()`（RSI:113-121）。
8. 本条消息在轮转后写入：需要轮转时 `new_size = formatted.size()`，随后 `write` 并更新 `current_size_`（RSI:116-124）。
9. `max_files == 0` 时不执行重命名循环，无编号备份，但 `reopen(true)` 仍截断活动文件（RSI:143-165）。
10. `rotate_()` 先 `close()`，从 `i = max_files_` 递减到 1，把 `index i-1` 重命名为 `index i`，源不存在则跳过（RSI:138-149）。
11. `rename_file_()` 先删目标（忽略返回值）再 rename；失败睡眠 100ms 重试，再失败则 `reopen(true)`、清零并抛带源名/目标名/errno 的异常（RSI:150-176）。
12. `set_max_size(0)`、`set_max_files(>200000)` 抛异常（RSI:78-99）。

## 2. 按日 / 按小时 sink

13. 默认日命名 `{basename}_{YYYY}-{MM}-{DD}{ext}`（DS:29-37）；格式串版本交给 `std::put_time`，支持其占位符（DS:40-58）。
14. Windows + `SPDLOG_WCHAR_FILENAMES` 时日格式用 `std::wstringstream`，否则 `std::stringstream`（DS:50-57）。
15. 构造校验 `rotation_hour ∈ 0..23`、`rotation_minute ∈ 0..59`，否则抛异常（DS:85-88）。
16. 构造按当前时间计算文件名并 `open(filename, truncate_)`，再设下次轮转点；`max_files_ > 0` 时初始化文件名队列（DS:90-97）。
17. 时间转换：`log_clock::time_point → to_time_t() → os::localtime()` 得本地 `tm`（DS:146-149）。
18. 下次轮转点：本地 `tm` 设 `tm_hour=rotation_h_`、`tm_min=rotation_m_`、`tm_sec=0`，`mktime`；若不大于当前时间则加 24 小时（DS:151-162）。
19. `sink_it_` 用消息时间判断 `msg.time >= rotation_tp_`，触发时先用 `msg.time` 计算新文件名并打开、更新轮转点，再格式化写入本条消息（DS:106-116）。
20. 旧文件清理在写入之后执行（删除可能抛异常，故放最后）（DS:118-121）。
21. `max_files == 0` 时不初始化队列、不删除旧文件；队列从当前时间向前按 24 小时回退，遇不存在文件停止；删除失败恢复当前文件名并抛异常（DS:61-66, 95-97, 119-121, 127-182）。
22. 默认小时命名 `{basename}_{YYYY}-{MM}-{DD}_{HH}{ext}`（HS:24-36）。
23. 小时轮转点：本地 `tm` 设 `tm_min=0`、`tm_sec=0`，`mktime`，若不大于当前时间则加 1 小时；边界为本地整点（HS:120-135）。
24. 构造打开当前小时文件，并以 `file_helper_.size() == 0` 设 `remove_init_file_`；写入前无条件置 false（HS:49-67, 87）。
25. 触发轮转时若 `remove_init_file_` 为真则关闭并删除当前文件；随后按 `msg.time` 计算新名、打开、更新轮转点（HS:75-86）。
26. `max_files == 0` 时不建队列/不删旧文件；队列按 1 小时回退，删除在写入后；失败抛异常并恢复队列（HS:38-43, 64-66, 93-95, 101-118, 137-155）。
27. 日/小时 sink 均注明：之前运行留下的旧文件不会被删除，轮转与删除只发生在程序运行期间（DS:65-66；HS:42-43）。

## 3. 打开模式、权限、flush

28. `basic_file_sink` 构造直接 `file_helper_.open(filename, truncate)`；写入只格式化后 `write`，无每次写入 flush（BSI:16-22, 29-40）。
29. `file_helper::open()` 先 `close()`，实际写日志始终用 `"ab"`；`truncate=true` 时另开临时 `"wb"` 截断（FHI:26-49）。
30. 打开尝试 5 次，间隔 10ms；每次先 `create_dir(dir_name(fname))` 且不检查返回值（FH:50-51；FHI:33-58）。
31. `file_event_handlers` 有 before_open/after_open/before_close/after_close 四个回调，**不是**错误处理器（C:328-339）。
32. POSIX + `SPDLOG_PREVENT_CHILD_FD` 用 `open(O_CREAT|O_WRONLY|O_CLOEXEC|mode, 0644)` 再 `fdopen`，`"ab"` 选 `O_APPEND`；否则纯 `fopen`（OSI:137-150）。
33. Windows 用 `_wfsopen`/`_fsopen`，共享 `_SH_DENYNO`；`SPDLOG_PREVENT_CHILD_FD` 清除继承标志（OSI:121-135）。
34. `create_dir()` 递归创建目录：POSIX `mkdir(0755)`，Windows `_wmkdir`/`_mkdir`（OSI:468-517）。
35. `flush()` 调 `std::fflush(fd_)`；`sync()` 调 `os::fsync(fd_)`；四个文件 sink 的 `flush_()` 只调 flush，未调 sync（FHI:71-81；RSI:127-130；DS:124；HS:98；BSI:42-45）。
36. logger 在消息级别达到 `flush_level_` 后逐 sink flush 并捕获异常（LHI:143-151, 166-169）；仅 rotating 在估算超限时额外 flush 并读真实大小（RSI:113-121）。

## 4. 错误路径

37. sink/helper 层失败一律 `throw_spdlog_ex` 或 `SPDLOG_THROW`，不直接调 `err_handler`（RSI:159-161；DS:177-178；HS:150-151；FHI:60-61, 73-79, 104, 110）。
38. 经 logger 写日志时 `sink->log()` 被 `SPDLOG_LOGGER_CATCH(msg.source)` 包围，标准异常交给 `err_handler_`（LHI:135-140；LH:30-39）。
39. 经 logger flush 时同样捕获（LHI:148-152）；未知异常调 err_handler 后重抛（LH:40-46）。
40. 自定义错误处理器由 `logger::set_error_handler()` 设置；默认限频打印 stderr（每秒最多一次）（LHI:111-114, 171-196）。
41. `SPDLOG_NO_EXCEPTIONS` 下 `SPDLOG_THROW` 打印 fatal 后 `abort()`（C:107-121）。
42. 打开失败：`fopen_s` 返回 true 时重试后抛 `Failed opening file ... for writing` 带 errno（OSI:120-154；FHI:36-61）。
43. 构造期打开失败不在 logger 的 catch 范围内；轮转期间触发的打开失败会被捕获交 `err_handler_`（BSI:16-22；RSI:43；DS:90-93；HS:58-62；LHI:135-151）。
44. 写失败：`fd_ == nullptr` 直接返回；`fwrite_bytes` 失败抛 `Failed writing to file ...`（FHI:98-105）。
45. flush/sync 失败抛 `Failed flush to file ...` / `Failed to fsync file ...`；`size()` 失败也抛（FHI:71-81, 108-113；OSI:197-240）。
46. 仅 rotating 有重命名失败路径；日/小时不重命名（RSI:150-176；DS:106-121；HS:75-95）。

## 5. 平台分支

47. `filename_t` 仅在 `_WIN32 && SPDLOG_WCHAR_FILENAMES` 下为 `wstring`（C:131-139）。
48. `SPDLOG_EOL`：Windows `\r\n`、POSIX `\n`；分隔符 Windows `\\/`、POSIX `/`（OS:23-45）。
49. `localtime/gmtime`：Windows `_s` 版本，其他 `_r` 版本（OSI:87-117）；`remove/rename` Windows wchar 用 `_w` 版本（OSI:156-174）。
50. `path_exists` Windows `_wstat/_stat`、POSIX `stat`（OSI:176-189）；`filesize` 按平台选 `fstat64`/`fstat`/`_filelengthi64`（OSI:197-240）。
51. `fsync` Windows `FlushFileBuffers`，POSIX `::fsync`；`fwrite_bytes` 在 `SPDLOG_FWRITE_UNLOCKED` 下用非锁定版本（OSI:546-566）。
52. `sleep_for_millis` Windows `::Sleep`、POSIX `sleep_for`（OSI:334-342）。
53. `os-inl.h` 还按 `_WIN32`、`__MINGW32__`、`__linux__`、`SPDLOG_CLOCK_COARSE`、`_AIX`、`__APPLE__`、`SPDLOG_NO_TZ_OFFSET`、`SPDLOG_NO_TLS` 等选择实现（OSI:24-65, 75-86, 246-269, 324-332）。
