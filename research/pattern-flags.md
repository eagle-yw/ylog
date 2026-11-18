# spdlog 1.17.0 pattern flag 全集与精确语义

研究工单：[spdlog pattern flag 全集与精确语义](https://github.com/eagle-yw/ylog/issues/2)。一手来源为本仓库 `tmp/spdlog-1.17.0/include/spdlog/`；每条事实均附源码行号。本文只记录事实，不含设计建议。

路径缩写（均为绝对路径前缀 `/home/eagle/workspace/Playground/cpp/Ylog/tmp/spdlog-1.17.0/include/spdlog/`）：

- `PFH` = `pattern_formatter.h`
- `PFI` = `pattern_formatter-inl.h`
- `FOH` = `formatter.h`
- `COMH` = `common.h`
- `FMTHELH` = `details/fmt_helper.h`
- `OSI` = `details/os-inl.h`
- `BSH` / `BSI` = `sinks/base_sink.h` / `sinks/base_sink-inl.h`
- `LOGI` = `logger-inl.h`；`REGI` = `details/registry-inl.h`
- `SINKH` = `sinks/sink.h`；`SPDH` / `SPDI` = `spdlog.h` / `spdlog-inl.h`；`DSH` = `sinks/dist_sink.h`

## 1. flag 全集

1. `%+` 注册 `full_formatter` 并置 `need_localtime_ = true`；输出 `[年-月-日 时:分:秒.毫秒]`、可选 `[logger]`、`[level]`、可选 `[短文件名:行号]`、可选 `[MDC]`、payload；level 区间设为颜色范围（PFI:1035-1038, 856-937）。
2. `%n` 输出 logger 名；`%l` 完整 level 名；`%L` 单字符短 level。默认名 trace/debug/info/warning/error/critical/off，短名 T/D/I/W/E/C/O，可由 `SPDLOG_LEVEL_NAMES`、`SPDLOG_SHORT_LEVEL_NAMES` 覆盖（PFI:1040-1051, 103-141；COMH:256-277）。
3. `%v` 输出消息正文（PFI:596-606, 1057-1059）。
4. `%t` 线程 id；`%P` 每次格式化调用 `os::pid()` 输出 PID（PFI:567-579, 581-594, 1053-1055, 1174-1176）。
5. `%a`/`%A` 缩写/完整星期；`%b`/`%h` 缩写月份、`%B` 完整月份（PFI:151-182, 184-217, 1061-1080）。
6. `%c` 日期时间字段，尺寸按 24；拼接为 `缩写星期 缩写月 日 时:分:秒 年`，日不补零（PFI:219-245）。
7. `%C` 两位年（`tm_year % 100`）；`%Y` 四位年（PFI:248-293, 1087-1095）。
8. `%D` 与 `%x` 等价，输出 `MM/DD/YY`（PFI:262-279, 1097-1101）。
9. `%m` 月、`%d` 日，均补两位（PFI:295-321, 1103-1111）。
10. `%H` 24 小时制；`%I` 仅在 `tm_hour > 12` 时减 12，故 0 点输出 `00`、12 点输出 `12`（PFI:147-149, 323-349, 1113-1121）。
11. `%M` 分钟、`%S` 秒，均补两位（PFI:351-377, 1123-1131）。
12. `%e`/`%f`/`%F` 为毫秒/微秒/纳秒的整秒余数，补 3/6/9 位；`%E` 为 epoch 起整秒数（PFI:379-439, 1133-1147；FMTHELH:127-137）。
13. `%p` 输出 AM/PM；`%r` 输出 12 小时时钟 `hh:mm:ss AM/PM`（PFI:441-474, 1149-1157）。
14. `%R` 输出 `HH:MM`；`%T` 与 `%X` 等价输出 `HH:MM:SS`（PFI:476-510, 1159-1168）。
15. `%z` 输出 `±HH:MM` 偏移；UTC 模式固定 `+00:00`；`SPDLOG_NO_TZ_OFFSET` 下固定 `+??:??`（PFI:512-565, 1169-1173；OSI:246-269）。
16. `%^`/`%$` 把 `color_range_start/end` 设为当前 `dest.size()`，不写转义序列，须成对使用（PFI:635-654, 1178-1184）。
17. `%@` 输出 `source.filename:line`；`%g` 完整源文件名；`%s` 去目录短名（PFI:656-743, 1186-1199）。
18. `%#` 源行号；`%!` 源函数名；`source_loc::empty()`（`line <= 0`）时只走 padding 不输出（PFI:745-780, 1201-1209；COMH:315-326）。
19. `%%` 字面 `%`（PFI:608-619, 1211-1213）。
20. `%u`/`%i`/`%o`/`%O` 距上一条消息的纳秒/微秒/毫秒/秒数；`last_message_time_` 构造时为 `now()`，每次取 `max(delta, 0)` 后更新（PFI:782-804, 1215-1237）。
21. `%&` 输出 MDC `key:value`（空格分隔），仅未定义 `SPDLOG_NO_TLS` 时注册（PFI:806-847, 1239-1243；PFI:14-16；COMH:96-101）。
22. 未知 flag 不报语法错：无 truncate 时原样输出 `%` 加该字符；畸形 pattern 无异常或错误回调路径（PFI:1245-1264, 1276-1297, 1316-1350）。

## 2. padding / 对齐 / 截断

23. `padding_info` 有 `width_`、`side_`（left/right/center）、`truncate_`、`enabled_`；默认构造禁用（PFH:22-38）。
24. 前缀 `-` → right、`=` → center，其他默认 left；`-`/`=` 后不是数字则禁用 padding（PFI:1280-1297）。
25. 宽度上限 `max_width = 64`，`std::min` 截断；宽度后紧跟 `!` 才置 `truncate_`（PFI:1271-1314）。
26. left 在值前补空格；right 在值后补；center 前后各补一半；无 padding 用 `null_scoped_padder`（PFI:42-60, 68-77, 92-101, 1327-1334）。
27. 截断在 `scoped_padder` 析构时执行；补位字符只有空格，`%08l` 表示宽 8 左补空格，不是 0 填充（PFI:68-89, 1295-1313）。
28. `%.3v` 按字面输出：`.` 不消费，随后 `3`、`v` 作为普通字符（PFI:1280-1297, 1321-1344, 1245-1252）。
29. `%3!!` 是 `source_funcname` 兼容分支（PFI:1253-1262）。
30. `set_pattern()` 清空重建 `formatters_` 但保留 `custom_handlers_`（PFI:1007-1011, 1316-1320；PFH:99-100）。

## 3. 时间与缓存

31. `pattern_time_type` 只有 `local` / `utc`，默认 `local`（COMH:290-297；PFH:69-76）。
32. `get_time_()` 在 local 下调用 `os::localtime`，utc 下调用 `os::gmtime`（PFI:1015-1020）。
33. 置 `need_localtime_` 的 flag：`+ a A b/h B c C Y D/x m d H I M S p r R T/X z`（PFI:1035-1038, 1061-1173）。
34. 不置的 flag：`n l L t v e f F E P ^ $ @ s g # ! % u i o O &`（PFI:1040-1059, 1133-1243）。
35. `format()` 取 `msg.time` 整秒，仅在秒变化时更新 `cached_tm_`，所有 flag 共用；末尾追加 `eol_`（PFI:990-1005）。
36. `need_localtime(bool)` 只赋值；`clone()` 复制该标志；`set_pattern()` 重置后按 flag 重新置位（PFH:89-90；PFI:1013, 975-988, 1007-1011）。
37. 无 pattern 构造用 `%+` 并显式置 `need_localtime_ = true`（PFI:964-973）。
38. `full_formatter` 自身有二级缓存：`cached_datetime_` 仅整秒变化或为空时重建，毫秒每次重取（PFI:861-892, 939-945）。
39. `%z` 偏移有 10 秒缓存；UTC 直接 `+00:00`；非 Windows 用 `tm_gmtoff/60`，Windows 用 `mktime` 与 `_mkgmtime` 差（PFI:525-565；OSI:246-269）。
40. `os::localtime/gmtime` 返回 `std::tm`：Windows `localtime_s/gmtime_s`，其他 `localtime_r/gmtime_r`（OSI:87-112）。

## 4. 自定义 flag 与 sink 关系

41. 容器为 `std::unordered_map<char, std::unique_ptr<custom_flag_formatter>>`（PFH:65-72, 100）。
42. `add_flag` 签名：`template <typename T, typename... Args> pattern_formatter &add_flag(char flag, Args &&...args)`，内部 `make_unique<T>`（PFH:84-88）。
43. `custom_flag_formatter` 继承 `details::flag_formatter`，要求 `virtual std::unique_ptr<custom_flag_formatter> clone() const = 0`；`set_padding_info()` 写入 `padinfo_`（PFH:40-63）。
44. 分发先查自定义表：命中则每次 `clone()` handler、写入 padding 再压栈，自定义同字符 flag 优先于内置（PFI:1022-1034）。
45. `pattern_formatter::clone()` 克隆所有 handler；拷贝构造/赋值被删除，克隆是唯一复制路径（PFH:78-81；PFI:975-988）。
46. 每个 base sink 持有独立 `std::unique_ptr<formatter>`，默认新 `pattern_formatter()`；`set_pattern()` 新建替换，`set_formatter()` move 替换（BSH:36-44；BSI:16-23, 38-58）。
47. `logger::set_formatter()` 对除最后一个 sink 外使用 `clone()`；`set_pattern()` 构造 `pattern_formatter` 后走此路径（LOGI:70-87）。
48. 全局 `set_pattern()` 交给 registry 保存一份并由 logger 逐级 clone；`dist_sink` 也向子 sink clone（SPDH:54-60；SPDI:27-30；REGI:123-129；DSH:64-72）。

## 5. formatter 接口

49. `formatter` 只有虚析构、`format(const log_msg&, memory_buf_t&)`、`clone() const -> std::unique_ptr<formatter>`；无 `pattern()` 虚函数（FOH:11-16）。
50. `pattern_formatter` 有两个构造函数、`set_pattern(std::string)`、`need_localtime(bool = true)`（PFH:69-90）。
51. sink 接口有 `set_pattern(const std::string&)` 与 `set_formatter(std::unique_ptr<formatter>)`；全局 `set_pattern(std::string, pattern_time_type = local)`（SINKH:15-18；SPDH:57-60）。
52. 内部 `flag_formatter::format(const log_msg&, const std::tm&, memory_buf_t&)` 比外部 `formatter::format` 多 `std::tm` 参数（PFH:40-52）。
53. `memory_buf_t` 在 `SPDLOG_USE_STD_FORMAT` 下为 `std::string`，否则为 `fmt::basic_memory_buffer<char, 250>`（COMH:145-180）。
