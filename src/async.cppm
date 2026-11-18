export module ylog:async;

import std;
import :level;
import :message;
import :sink;
import :logger.base;
import :async.queue;
import ylog_detail_errors;

namespace ylog {

// 队列溢出策略。构造后不可变，避免热路径读取配置时产生竞态。
export enum class OverflowPolicy {
    Block,          // 队列满时生产者等待空位
    OverrunOldest,  // 队列满时覆盖最旧的一条消息
    DiscardNew,     // 队列满时丢弃新来的消息
};

// 默认 handler 的公开工厂：`AsyncLoggerConfig` 的默认值出现在导出类型的声明面上，
// 那里不能写 detail 限定名，所以把构造收进这个公开函数。实体在 `detail::errors`，
// 与各 sink 配置结构体的默认值共用同一份实现（ADR-0009）。
export auto default_error_handler() -> ErrorHandler {
    return ylog_detail_errors::make_default_err_handler();
}

// 字段顺序即指定初始化的书写顺序：sinks 放最后，调用方按名字、队列、handler 的顺序
// 写配置时不必回头补 sink 集合。
export struct AsyncLoggerConfig {
    std::string name{};  // Registry 创建时按名覆盖
    Level level = Level::Info;
    std::size_t queue_capacity = 8192;
    OverflowPolicy overflow_policy = OverflowPolicy::Block;
    ErrorHandler err_handler = default_error_handler();
    std::vector<AnySink> sinks{};  // 默认空：零 sink 合法，语义是丢弃
};

// 异步日志组件：调用线程只做级别检查、Message 构造与入队，
// 由独占的工作线程把消息投递给自己的 sink。
// 契约：不实现 Sink concept，也不修改 Logger；名称/级别/sink 集合与级别便捷方法
// 来自 detail::LoggerBase，这里只追加队列、工作线程与上报口径。
//
// 队列与 Flush 屏障的转换规则归 detail::QueueBarrier：本组件只负责「把消息交给队列」
// 与「把队列交出来的东西送到 sink」。锁内不做 sink I/O，也不调 err_handler。
//
// sink 集合在构造期定型，worker 启动后不存在「被追加」的可能，因此不需要封板断言；
// set_pattern 逐 sink 加 formatter 锁，可与 worker 的遍历并存。
// err_handler 在 worker 线程（sink 失败）与调用线程（丢弃/停止）上同步调用：worker 上
// 的 handler 不得重入本组件的 log()/flush()/stop()，否则等的是自己。见 ADR-0003。
export class AsyncLogger : public detail::LoggerBase<AsyncLogger> {
public:
    explicit AsyncLogger(AsyncLoggerConfig config)
        : LoggerBase(std::move(config.name), config.level, std::move(config.sinks)),
          queue_(make_queue(config.queue_capacity, config.overflow_policy)),
          err_handler_(std::move(config.err_handler)) {
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
    }

    AsyncLogger(const AsyncLogger&) = delete;
    auto operator=(const AsyncLogger&) -> AsyncLogger& = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    auto operator=(AsyncLogger&&) -> AsyncLogger& = delete;

    // 先请求停止，再由工作线程按 FIFO 排空队列，最后 join。
    ~AsyncLogger() { stop(); }

    // 显式停机：排空后返回。可重复调用；停止后的每次 log() 都立即失败并各自上报。
    //
    // 顺序固定为「先固定排空集合，再打断工作线程，最后 join」：排空集合必须是停止
    // 请求之前已入队的消息，否则持续生产者会在排空窗口里继续追加，worker 永远看不到
    // 空队列，join 挂住。join 用自己的锁，不能跨到队列锁上——worker 每轮都要队列锁。
    auto stop() -> void {
        queue_.begin_stop();
        worker_.request_stop();
        std::lock_guard lock(join_mutex_);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    auto queue_capacity() const -> std::size_t { return queue_.capacity(); }
    auto overflow_policy() const -> OverflowPolicy { return public_policy(queue_.policy()); }

    // 屏障：返回时先前入队的消息都已 write，且所有 sink 已 flush。
    //
    // 用「序号 + 条件变量」而不是往队列里塞控制消息：一条消息入队时拿到递增序号，
    // 这里记住当时的最大序号作为本次目标，等 worker 把 sink flush 到该序号之后返回。
    // 好处是队列里只有消息，长度严格受 capacity 约束，不随 flush 调用次数增长；
    // 并发 flush 各自等自己的目标，互不提前结清。
    auto flush() -> void {
        auto request = queue_.arm_barrier();
        switch (request.next) {
            case detail::barrier_request::kind::satisfied:
                return;  // 目标之内没有新消息，sink 也已经是 flush 过的状态
            case detail::barrier_request::kind::worker_gone:
                // 工作线程已退出，没人会把 sink flush 到目标：调用线程自己来。
                // 同一 sink 的并发 flush 由 AnySink 的 output_mutex_ 串行化。
                flush_sinks();
                return;
            case detail::barrier_request::kind::wait:
                queue_.await_barrier(request.target);
                return;
        }
    }

    // 核心日志入口。SourceLocation 默认参数在调用点捕获位置。
    // 上报在锁外进行：handler 可能在调用线程重入 log()，持锁回调会自锁。
    template <typename... Args>
    auto log(SourceLocation location,
             Level level,
             std::format_string<Args...> format,
             Args&&... args) -> void {
        if (!should_log(level)) {
            return;
        }
        auto message =
            Message(level, name(), location, std::format(format, std::forward<Args>(args)...));
        // Flush 阈值在入队前折算：入队成功后由队列把屏障目标推到这一条。
        auto reached_flush_level = message.level() >= flush_level();
        auto admitted = queue_.admit(std::move(message), reached_flush_level);
        EnqueueOutcome::from(std::move(admitted)).report(err_handler_);
    }

private:
    // 一次入队的结果。三种丢失互斥，共用「为什么丢 + 丢的是哪条」，
    // 上报因此收在一次调用里，log() 尾部不再三个 if 并列。
    struct EnqueueOutcome {
        enum class Loss { None, Overrun, Discarded, Rejected };

        Loss loss = Loss::None;
        std::string lost;  // 丢失条目的文本快照；入队成功时为空

        // 队列给出的后果 → 上报口径。丢失条目的文本由队列在消息还活着时拷出，
        // 这里不再碰 Message：入队这一支已经把消息搬走了。
        static auto from(detail::admission admitted) -> EnqueueOutcome {
            switch (admitted.result) {
                case detail::admission::outcome::overran:
                    return EnqueueOutcome{Loss::Overrun, std::move(admitted.lost)};
                case detail::admission::outcome::discarded:
                    return EnqueueOutcome{Loss::Discarded, std::move(admitted.lost)};
                case detail::admission::outcome::rejected:
                    return EnqueueOutcome{Loss::Rejected, std::move(admitted.lost)};
                case detail::admission::outcome::enqueued:
                    break;
            }
            return EnqueueOutcome{};  // 入队成功：没有要上报的
        }

        // 每次丢失都上报：spec 的通知面只有 err_handler，靠默认 handler 的限频
        // 控制输出量，而不是在这里把后续的丢失静默掉。
        auto report(const ErrorHandler& handler) const -> void {
            if (loss == Loss::None || !handler) {
                return;
            }
            ylog_detail_errors::safe_err_handler(handler, describe());
        }

    private:
        auto describe() const -> std::string {
            switch (loss) {
                case Loss::Overrun:
                    return std::format("溢出：覆盖最旧消息 {}（策略 OverrunOldest）", lost);
                case Loss::Discarded:
                    return std::format("溢出：丢弃新消息 {}（策略 DiscardNew）", lost);
                case Loss::Rejected:
                    return std::format("组件已停止，丢弃消息 {}", lost);
                case Loss::None:
                    break;
            }
            return {};
        }
    };

    // 配置期校验与「公开枚举 → 队列策略」的唯一映射点。两件事都在工作线程启动之前
    // 完成：构造失败不会留下半个组件（Registry 依赖这条）。
    static auto make_queue(std::size_t capacity, OverflowPolicy policy) -> detail::QueueBarrier {
        if (capacity == 0) {
            ylog_detail_errors::fail_config<std::invalid_argument>("queue_capacity 必须大于 0");
        }
        switch (policy) {
            case OverflowPolicy::Block:
                return detail::QueueBarrier(capacity, detail::admission_policy::block);
            case OverflowPolicy::OverrunOldest:
                return detail::QueueBarrier(capacity, detail::admission_policy::overrun_oldest);
            case OverflowPolicy::DiscardNew:
                return detail::QueueBarrier(capacity, detail::admission_policy::discard_new);
        }
        ylog_detail_errors::fail_config<std::invalid_argument>("未知的 overflow_policy");
        return detail::QueueBarrier(capacity, detail::admission_policy::block);  // 不可达
    }

    // 内部策略的公开拼写：组件不保留第二份配置，accessor 从这里翻译回去。
    static auto public_policy(detail::admission_policy policy) -> OverflowPolicy {
        switch (policy) {
            case detail::admission_policy::block:
                return OverflowPolicy::Block;
            case detail::admission_policy::overrun_oldest:
                return OverflowPolicy::OverrunOldest;
            case detail::admission_policy::discard_new:
                return OverflowPolicy::DiscardNew;
        }
        return OverflowPolicy::Block;  // 不可达：策略来自构造期校验过的配置
    }

    auto report(std::string text) -> void {
        if (err_handler_) {
            ylog_detail_errors::safe_err_handler(err_handler_, text);
        }
    }

    // 扇出交给 SinkSet：它负责守卫、跳过失败 sink、以及「连续失败只报一次」。
    // 无异常构建下 sink 不会抛，守卫退化为直接调用，失败由 sink 自己的 handler 上报。
    auto write_to_sinks(const Message& message) -> void {
        sink_set().write(message, [this](std::string_view why) {
            report(std::format("sink 写入失败：{}", why));
        });
    }

    // flush 失败算第三类触发点「某个 sink 投递失败」：数据没落盘同样是没送出去。
    // 这条路径每次都上报——只有显式 flush 才走到这儿，频率由调用方控制。
    auto flush_sinks() -> void {
        sink_set().flush(
            [this](std::string_view why) { report(std::format("sink flush 失败：{}", why)); });
    }

    // 工作线程：只执行队列交出来的步骤并回报。投递与落盘都在队列锁外，
    // 「排空到哪一条为止」也不再由这里判断——停止时队列先给完剩余的 deliver，
    // 最后才给 finished。
    auto run(std::stop_token token) -> void {
        for (;;) {
            auto step = queue_.next_work(token);
            switch (step.next) {
                case detail::work::kind::deliver:
                    write_to_sinks(*step.message);
                    queue_.note_delivered(step.sequence);
                    break;
                case detail::work::kind::flush:
                    flush_sinks();
                    queue_.note_flushed(step.target);
                    break;
                case detail::work::kind::finished:
                    return;
            }
        }
    }

    detail::QueueBarrier queue_;
    ErrorHandler err_handler_;
    std::mutex join_mutex_;
    std::jthread worker_;
};

}  // namespace ylog
