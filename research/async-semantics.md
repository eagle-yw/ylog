# spdlog 1.17.0 异步线程池、溢出策略与生命周期语义

研究工单：[spdlog 异步线程池与生命周期语义清单](https://github.com/eagle-yw/ylog/issues/3)。一手来源为 `tmp/spdlog-1.17.0/include/spdlog/`，每条事实附源码行号；只记录事实，不含设计建议。

路径缩写（前缀 `/home/eagle/workspace/Playground/cpp/Ylog/tmp/spdlog-1.17.0/include/spdlog/`）：`AS`=async.h，`ALH`/`ALI`=async_logger.h/-inl.h，`TPH`/`TPI`=details/thread_pool.h/-inl.h，`Q`=details/mpmc_blocking_q.h，`CQ`=details/circular_q.h，`RH`/`RIH`=details/registry.h/-inl.h，`LH`/`LIH`=logger.h/-inl.h，`CM`=common.h，`MB`=details/log_msg_buffer.h，`SI`=spdlog-inl.h。

## 1. async_msg 与所有权

1. `async_msg_type` 恰有三个值：`log`、`flush`、`terminate`；默认 `log`（TPH:23-29）。
2. `async_msg` 继承 `log_msg_buffer`，只可移动（拷贝删除，move 默认）（TPH:27-35, 50-53）。
3. `worker_ptr` 为 `std::shared_ptr<async_logger>`，每条消息持有 logger 强引用（TPH:21, 29, 56-64）。
4. 仅含类型的构造委托 `nullptr` logger，故 `terminate` 消息不持有 logger（TPH:66-67）。
5. 异步 `sink_it_()` / `flush_()` 把 `shared_from_this()` 交给 `post_log` / `post_flush`（ALI:33-49；ALH:33-35）。
6. `async_logger` 只以 `weak_ptr<thread_pool>` 持有线程池；消息以 `shared_ptr` 持有 logger（ALH:42, 45, 67；TPH:29）。
7. 源码注释：logger 在队列中仍有消息时删除，实际析构延后到消息处理完（AS:7-15）。
8. `log_msg_buffer` 自带 `memory_buf_t buffer`，保存原本指向栈的 `string_view`，避免异步消费悬空（MB:11-20）。

## 2. 溢出策略与队列默认值

9. 策略语义：`block` 等到有空间；`overrun_oldest` 满时丢弃队首；`discard_new` 满时丢弃新消息；枚举注释称默认 `block`（ALH:21-27）。
10. `async_logger` 三个构造函数默认实参均为 `block`（ALH:38-56）。
11. 映射：`block → q_.enqueue()`，`overrun_oldest → q_.enqueue_nowait()`，`discard_new → q_.enqueue_if_have_room()`；其他值 `assert(discard_new)`（TPI:79-88）。
12. `enqueue()` 在锁内 `pop_cv_.wait(!full())`，解锁后 `push_cv_.notify_one()`（Q:31-38）。
13. `enqueue_nowait()` 不检查满，直接 `push_back`；`circular_q::push_back()` 在 `tail_ == head_` 时前移 `head_` 并递增 `overrun_counter_`——保留新消息、覆盖最旧（Q:40-47；CQ:45-57）。
14. `enqueue_if_have_room()` 有空间才入队并通知，否则只递增原子 `discard_counter_`（Q:49-64）。
15. `overrun_counter()` / `discard_counter()` 与各自 reset 方法存在（Q:150-167；TPI:69-75）。
16. `circular_q(max_items)` 分配 `max_items + 1` 槽，有效容量 `max_items`（CQ:28-31, 87-93）。
17. 默认队列大小 `8192`；未提供线程池的工厂创建 `thread_pool(default_async_q_size, 1U)`（AS:28, 31-48）。
18. `async_factory` = `block` 别名，`async_factory_nonblock` = `overrun_oldest` 别名；无 `discard_new` 的同名工厂（AS:34, 58-59）。

## 3. 线程池

19. 队列为 `mpmc_blocking_queue<async_msg>`（TPH:70-73；Q:6, 22-27）。
20. 构造先建队列，再校验线程数，最后创建线程（TPI:16-32）。
21. 线程数合法范围 `1..1000`；`0` 或 `>1000` 抛 `spdlog_ex` 并给出范围消息（TPI:21-25）。
22. 每线程依次 `on_thread_start()` → `worker_loop_()` → `on_thread_stop()`（TPI:16-41）。
23. 析构为每线程入队一条 `block` 策略的 `terminate`，再逐线程 `join()`，包在 `SPDLOG_TRY/CATCH_STD` 中（TPI:43-55）。
24. `worker_loop_()` 反复 `process_next_msg_()` 到其返回 false；`terminate` 分支直接 false，不访问空 `worker_ptr`（TPI:91-122）。
25. `post_log()` 构造 `log` 消息并移入 logger shared_ptr 与消息，按策略入队（TPI:57-62）。
26. `post_flush()` 构造 `flush` 消息携带 logger shared_ptr，按同策略入队（TPI:64-67）。
27. 后端 `log` 调 `worker_ptr->backend_sink_it_()`；`flush` 调 `backend_flush_()`（TPI:102-110）。
28. 工作线程无超时 `q_.dequeue()` 阻塞，队列空停在 `push_cv_`，不轮询（TPI:91-100；Q:31-38, 81-90）。

## 4. 日志路径与错误回调

29. 同步/异步共用 `log* → log_() → log_it_()` 前半段；级别与 backtrace 检查在调用线程（LH:78-124, 316-335）。
30. 格式化在调用线程完成并构造 `log_msg`；格式化异常由 `SPDLOG_LOGGER_CATCH(loc)` 捕获（LH:316-335）。
31. `log_it_()` 级别允许时调虚方法 `sink_it_()`（LIH:124-133）。
32. 同步 `sink_it_()` 在调用线程逐 sink 检查级别并调用（LIH:135-146）。
33. 异步 `sink_it_()` 不调 sink，锁定线程池 `weak_ptr` 后 `post_log(shared_from_this(), ...)`；线程池消失时抛异常（ALI:33-43）。
34. 后端逐 sink 再次检查 `should_log()` 并调用；满足 flush 级别则 `backend_flush_()`（ALI:60-71）。
35. 同步 `flush()` 直接逐 sink；异步 `flush()` 入队，由工作线程执行（LIH:98, 148-153；ALI:45-55, 73-78）。
36. `SPDLOG_LOGGER_CATCH` 捕获 `std::exception` 并带源位置交给 `err_handler_`（LH:30-39）。
37. 未知异常调 `err_handler_("Rethrowing unknown exception in logger")` 后重抛（LH:40-43）。
38. 同步捕获点在 `log_()`、各 sink 调用、`flush_()`；异步另加入队与后端处理（LH:323-334；LIH:135-153；ALI:33-78）。
39. `err_handler_` 优先用自定义回调；默认向 stderr 输出，限频每秒最多一次（LIH:171-196）。
40. `registry::set_error_handler()` 立即替换所有已注册 logger 并作为新 logger 默认值（RIH:62-68, 165-171）。
41. 三种溢出路径本身只入队/覆盖/递增计数，未调用 `err_handler`（TPI:79-88；Q:31-64）。

## 5. registry 与全局线程池

42. 全局 registry 是函数局部静态单例（RIH:241-244）。
43. 线程池以 `shared_ptr<thread_pool> tp_` 存于 registry，`set_tp/get_tp` 由 `recursive_mutex tp_mutex_` 保护（RH:52-54, 92, 111-119；RIH:113-121, 218）。
44. 异步工厂在锁内读取全局池，为空则创建 `(8192, 1)` 写回，所有该工厂创建的 logger 共享（AS:31-53）。
45. `init_thread_pool(q_size, thread_count, ...)` 调 `registry::set_tp()`；`spdlog::thread_pool()` 从同一 registry 读（AS:75-98）。
46. `registry::shutdown()` 顺序：停周期 flush 线程 → `drop_all()` → `tp_.reset()`（RIH:203-216；SI:74）。
47. `initialize_logger()` 设置 formatter、错误回调、级别、flush 级别、backtrace 等（RIH:62-84）。

## 6. 条件编译与平台分支

48. `SPDLOG_HEADER_ONLY` 触发各头文件包含 `-inl.h`（ALH:72-74；TPH:115-117；RH:129-131；LH:377-379；CM:404-406）。
49. `SPDLOG_COMPILED_LIB` 取消 header-only 并定义 `SPDLOG_INLINE`；共享库与 `_WIN32` 决定导出宏（CM:28-48）。
50. `SPDLOG_NO_EXCEPTIONS` 下 `SPDLOG_TRY` 为空、`SPDLOG_THROW` 打印后 `abort()`、`SPDLOG_CATCH_STD` 与 `SPDLOG_LOGGER_CATCH` 为空（CM:107-121；LH:30-46）。
51. `mpmc_blocking_queue` 在 `__MINGW32__` 下为死锁避免在持锁期间通知（Q:29, 92-95）。
52. `_MSC_VER <= 1800` 下 `async_msg` 手写 move；其他默认（TPH:37-53）。
53. 未禁用默认 logger 时按平台创建 `wincolor_stdout_sink_mt` 或 `ansicolor_stdout_sink_mt`（RIH:15-22, 35-47）。
54. `SPDLOG_USE_STD_FORMAT` 选择 `std::format_string`，否则 fmt；受 `__cpp_lib_format >= 202207L`、`FMT_VERSION`、`FMT_USE_CONSTEXPR` 影响（CM:19-26, 50-86, 145-211；LH:325-329）。
55. `SPDLOG_WCHAR_TO_UTF8_SUPPORT` 在非 Windows 触发 `#error`（CM:213-217）。
56. 默认错误输出在 R 环境下用 `REprintf`，否则 `fprintf(stderr, ...)`；`SPDLOG_NO_ATOMIC_LEVELS` 决定空操作原子或 `std::atomic<int>`（LIH:189-195；CM:225-229）。
