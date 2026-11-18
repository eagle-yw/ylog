module;

// assert 是宏，`import std;` 带不进来；内部不变量要用它，只能在全局模块片段里取。
#include <cassert>

export module ylog:async;

import std;
import :level;
import :message;
import :formatter;
import :sink;
import :sink.stdout;
import ylog_detail_errors;

namespace ylog {

// 热路径失败的上报接口：消息是借用视图，只在调用期间有效。
export using ErrorHandler = std::function<void(std::string_view)>;

// 队列溢出策略。构造后不可变，避免热路径读取配置时产生竞态。
export enum class OverflowPolicy {
    Block,          // 队列满时生产者等待空位
    OverrunOldest,  // 队列满时覆盖最旧的一条消息
    DiscardNew,     // 队列满时丢弃新来的消息
};

namespace detail {

// 默认上报：写 stderr，约每秒一条，其余计数后静默丢弃（下次放行时附带抑制条数）。
// 状态放在 shared_ptr 里：ErrorHandler 要求可拷贝，拷贝之间共享同一份限频。
class DefaultErrorHandler {
public:
    auto operator()(std::string_view text) const -> void {
        constexpr auto interval_ns = std::chrono::seconds(1).count() * 1'000'000'000LL;
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        auto last = state_->last_report_ns.load(std::memory_order_relaxed);
        if (last != 0 && now_ns - last < interval_ns) {
            state_->suppressed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!state_->last_report_ns.compare_exchange_strong(last, now_ns,
                                                            std::memory_order_relaxed)) {
            state_->suppressed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto suppressed = state_->suppressed.exchange(0, std::memory_order_relaxed);
        if (suppressed > 0) {
            std::println(std::cerr, "ylog: {}（此前已抑制 {} 条同类上报）", text, suppressed);
        } else {
            std::println(std::cerr, "ylog: {}", text);
        }
    }

private:
    struct State {
        std::atomic<long long> last_report_ns{0};
        std::atomic<long long> suppressed{0};
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// sink 调用守卫：把异常收敛成一次上报。
// 必须是模板：-fno-exceptions 下 try/catch 的语义检查发生在实例化期，
// 被 if constexpr 丢弃的分支才不会报“exception handling disabled”。
template <typename Action, typename OnFailure>
inline auto guard_sink_call(Action&& action, OnFailure&& on_failure) -> void {
    if constexpr (ylog_detail_errors::exceptions_enabled) {
        try {
            std::invoke(action);
        } catch (const std::exception& failure) {
            std::invoke(on_failure, std::string_view(failure.what()));
        } catch (...) {
            std::invoke(on_failure, std::string_view("未知异常"));
        }
    } else {
        std::invoke(action);
    }
}

}  // namespace detail

// 默认 handler 的公开工厂：`AsyncLoggerConfig` 的默认值出现在导出类型的声明面上，
// 那里不能写 detail 限定名，所以把构造收进这个公开函数。
export auto default_error_handler() -> ErrorHandler { return detail::DefaultErrorHandler{}; }

export struct AsyncLoggerConfig {
    std::string name;
    Level level = Level::Info;
    std::size_t queue_capacity = 8192;
    OverflowPolicy overflow_policy = OverflowPolicy::Block;
    ErrorHandler err_handler = default_error_handler();
};

// 异步日志组件：调用线程只做级别检查、Message 构造与入队，
// 由独占的工作线程把消息投递给自己的 sink。
// 契约：不实现 Sink concept，也不修改 Logger；sink 集合只在配置期变更。
export class AsyncLogger {
public:
    explicit AsyncLogger(AsyncLoggerConfig config)
        : name_(std::move(config.name)),
          level_(config.level),
          queue_capacity_(config.queue_capacity),
          overflow_policy_(config.overflow_policy),
          err_handler_(std::move(config.err_handler)) {
        validate_config();
        sinks_.push_back(SinkSlot{AnySink{StdoutSink{}}});
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
    // stopping_ 先于 request_stop 置位：「要排空的集合」必须是停止请求之前已入队的
    // 消息。否则持续生产者会在排空窗口里继续追加，worker 永远看不到空队列，join 挂住。
    auto stop() -> void {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        worker_.request_stop();
        cv_ready_.notify_all();
        cv_space_.notify_all();
        std::lock_guard lock(join_mutex_);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    auto name() const -> std::string_view { return name_; }
    auto level() const -> Level { return level_.load(std::memory_order_relaxed); }
    auto set_level(Level target) -> void { level_.store(target, std::memory_order_relaxed); }

    auto queue_capacity() const -> std::size_t { return queue_capacity_; }
    auto overflow_policy() const -> OverflowPolicy { return overflow_policy_; }

    // 追加 sink，可为其单独指定 pattern（formatter 归属 sink）。
    template <Sink T>
    auto append_sink(T&& sink, std::string_view pattern = default_pattern) -> void {
        sinks_.push_back(
            SinkSlot{AnySink(std::forward<T>(sink), std::make_unique<PatternFormatter>(pattern))});
    }

    // 追加 sink 并注入配置好的 formatter（可带自定义 flag）。
    template <Sink T>
    auto append_sink(T&& sink, std::unique_ptr<Formatter> formatter) -> void {
        sinks_.push_back(SinkSlot{AnySink(std::forward<T>(sink), std::move(formatter))});
    }

    // 为所有 sink 设置 pattern（扇出）。
    auto set_pattern(std::string_view pattern) -> void {
        for (auto& slot : sinks_) {
            slot.sink.set_pattern(pattern);
        }
    }

    // 屏障：返回时先前入队的消息都已 write，且所有 sink 已 flush。
    //
    // 用「序号 + 条件变量」而不是往队列里塞控制消息：一条消息入队时拿到递增序号，
    // 这里记住当时的最大序号作为本次目标，等 worker 把 sink flush 到该序号之后返回。
    // 好处是队列里只有消息，长度严格受 capacity 约束，不随 flush 调用次数增长；
    // 并发 flush 各自等自己的目标，互不提前结清。
    auto flush() -> void {
        std::unique_lock lock(mutex_);
        auto target = next_sequence_;
        if (worker_finished_) {
            // 工作线程已退出，没人会把 sink flush 到目标：调用线程自己来。
            // 同一 sink 的并发 flush 由 AnySink 的 output_mutex_ 串行化。
            lock.unlock();
            flush_sinks();
            return;
        }
        if (flushed_sequence_ >= target) {
            return;  // 目标之内没有新消息，sink 也已经是 flush 过的状态
        }
        flush_target_ = std::max(flush_target_, target);
        cv_ready_.notify_one();
        cv_flushed_.wait(lock, [this, target] { return flushed_sequence_ >= target; });
    }

    // 核心日志入口。SourceLocation 默认参数在调用点捕获位置。
    // 非阻塞策略的上报在锁外进行：handler 可能重入日志，持锁回调会自锁。
    template <typename... Args>
    auto log(SourceLocation location,
             Level level,
             std::format_string<Args...> format,
             Args&&... args) -> void {
        if (!should_log(level)) {
            return;
        }
        auto message =
            Message(level, name_, location, std::format(format, std::forward<Args>(args)...));
        auto overrun = std::string{};  // OverrunOldest 覆盖掉的最旧消息
        auto discarded = false;        // DiscardNew 丢弃的新消息
        auto rejected_after_stop = false;
        auto enqueued = false;
        {
            std::unique_lock lock(mutex_);
            if (stopping_) {
                rejected_after_stop = true;
            } else {
                switch (overflow_policy_) {
                    case OverflowPolicy::Block:
                        // 停止请求也要唤醒等待容量的生产者：让它放弃入队，而不是等到
                        // 队列腾出空位——排空期间队列可能永远不空。
                        cv_space_.wait(
                            lock, [this] { return stopping_ || queue_.size() < queue_capacity_; });
                        if (stopping_) {
                            rejected_after_stop = true;
                        } else {
                            enqueue(std::move(message));
                            enqueued = true;
                        }
                        break;
                    case OverflowPolicy::OverrunOldest:
                        if (queue_.size() >= queue_capacity_) {
                            overrun = drop_oldest_message_locked();
                        }
                        enqueue(std::move(message));
                        enqueued = true;
                        break;
                    case OverflowPolicy::DiscardNew:
                        if (queue_.size() >= queue_capacity_) {
                            discarded = true;
                        } else {
                            enqueue(std::move(message));
                            enqueued = true;
                        }
                        break;
                }
            }
        }
        if (enqueued) {
            cv_ready_.notify_one();
        }
        if (!overrun.empty()) {
            report(std::format("溢出：覆盖最旧消息 {}（策略 OverrunOldest）", overrun));
        }
        if (discarded) {
            report(std::format("溢出：丢弃新消息 {}（策略 DiscardNew）", message.formatted()));
        }
        if (rejected_after_stop) {
            // 每次拒绝都上报：spec 的通知面只有 err_handler，靠默认 handler 的限频
            // 控制输出量，而不是在这里把后续的丢失静默掉。
            report(std::format("组件已停止，丢弃消息 {}", message.formatted()));
        }
    }

    // 各级别便捷方法。第一个参数 {} 自动捕获调用点。
    template <typename... Args>
    auto trace(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        log(loc, Level::Trace, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    auto debug(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        log(loc, Level::Debug, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    auto info(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        log(loc, Level::Info, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    auto warning(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        log(loc, Level::Warning, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    auto error(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        log(loc, Level::Error, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    auto critical(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        log(loc, Level::Critical, fmt, std::forward<Args>(args)...);
    }

private:
    // 队列里只有消息：序号在入队时分配，flush() 用它表达「刷到哪一条为止」。
    struct QueuedMessage {
        Message message;
        std::size_t sequence;
    };

    auto should_log(Level msg_level) const -> bool {
        return msg_level >= level_.load(std::memory_order_relaxed);
    }

    auto validate_config() const -> void {
        if (queue_capacity_ == 0) {
            ylog_detail_errors::fail_config<std::invalid_argument>("queue_capacity 必须大于 0");
        }
        switch (overflow_policy_) {
            case OverflowPolicy::Block:
            case OverflowPolicy::OverrunOldest:
            case OverflowPolicy::DiscardNew:
                break;
            default:
                ylog_detail_errors::fail_config<std::invalid_argument>("未知的 overflow_policy");
        }
    }

    auto enqueue(Message message) -> void {
        queue_.push_back(QueuedMessage{std::move(message), next_sequence_++});
        // 不变量：队列里只有消息，长度受容量约束，flush 不占用队列空间。
        assert(queue_.size() <= queue_capacity_);
    }

    // 调用方必须持有 mutex_。覆盖最旧的一条，返回它的文本供上报。
    auto drop_oldest_message_locked() -> std::string {
        auto text = std::string(queue_.front().message.formatted());
        queue_.pop_front();
        return text;
    }

    auto report(std::string text) -> void {
        if (err_handler_) {
            ylog_detail_errors::safe_err_handler(err_handler_, text);
        }
    }

    // 单个 sink 失败只上报并跳过：异常不得逃出工作线程，也不影响其它 sink。
    // 连续失败只上报一次，避免坏 sink 在每条消息上刷屏；解除条件是它下一次写成功。
    // 无异常构建下 sink 不会抛，守卫退化为直接调用，失败由 sink 自己的 handler 上报。
    auto write_to_sinks(const Message& message) -> void {
        for (auto& slot : sinks_) {
            auto failure = std::string{};
            detail::guard_sink_call(
                [&slot, &message] { slot.sink.write(message); },
                [&failure](std::string_view why) { failure = std::string(why); });
            if (failure.empty()) {
                slot.failing = false;
                continue;
            }
            if (slot.failing) {
                continue;
            }
            slot.failing = true;
            report(std::format("sink 写入失败：{}", failure));
        }
    }

    // flush 失败算第三类触发点「某个 sink 投递失败」：数据没落盘同样是没送出去。
    // 这里每次都上报——只有显式 flush 才走到这儿，频率由调用方控制。
    auto flush_sinks() -> void {
        for (auto& slot : sinks_) {
            detail::guard_sink_call(
                [&slot] { slot.sink.flush(); },
                [this](std::string_view why) { report(std::format("sink flush 失败：{}", why)); });
        }
    }

    // 工作线程：投递队列，按需为目标序号落盘；停止请求后先把队列排空，再退出。
    auto run(std::stop_token token) -> void {
        for (;;) {
            std::unique_lock lock(mutex_);
            cv_ready_.wait(lock, token,
                           [this] { return !queue_.empty() || flush_target_ > flushed_sequence_; });

            if (!queue_.empty()) {
                auto item = std::move(queue_.front());
                queue_.pop_front();
                lock.unlock();
                cv_space_.notify_one();
                write_to_sinks(item.message);
                lock.lock();
                processed_sequence_ = item.sequence + 1;
                continue;
            }
            if (flush_target_ > flushed_sequence_) {
                // 队列空时 processed_sequence_ 一定追平 next_sequence_：覆盖策略只删队首，
                // 最新入队的一条总会落地；所以这里不该出现「目标还没写出」的状态。
                assert(processed_sequence_ >= flush_target_);
                // 目标之内的消息都写完了：落盘一次，然后把这之前的序号标记为已 flush。
                auto target = flush_target_;
                lock.unlock();
                flush_sinks();
                lock.lock();
                flushed_sequence_ = std::max(flushed_sequence_, target);
                cv_flushed_.notify_all();
                continue;
            }
            if (token.stop_requested()) {
                worker_finished_ = true;
                lock.unlock();
                // 唤醒可能仍卡在容量上或 flush 上的调用方，让它们走停止分支而不是继续睡。
                cv_space_.notify_all();
                cv_ready_.notify_all();
                cv_flushed_.notify_all();
                return;
            }
        }
    }

    std::string name_;
    std::atomic<Level> level_{Level::Info};
    std::size_t queue_capacity_{8192};
    OverflowPolicy overflow_policy_{OverflowPolicy::Block};
    ErrorHandler err_handler_;

    // sink 与它的失败状态放在一起：连续失败只在进入失败状态时上报一次。
    struct SinkSlot {
        AnySink sink;
        bool failing{false};
    };

    std::vector<SinkSlot> sinks_;
    std::mutex mutex_;
    std::condition_variable_any cv_ready_;
    std::condition_variable_any cv_space_;
    std::condition_variable_any cv_flushed_;
    std::deque<QueuedMessage> queue_;
    std::size_t next_sequence_{0};       // 下一个待分配的序号 = 已入队条数
    std::size_t processed_sequence_{0};  // 已被 worker 写出的条数
    std::size_t flush_target_{0};        // 当前最高的 flush 目标（0 表示无请求）
    std::size_t flushed_sequence_{0};    // 已确认落盘的条数
    bool stopping_{false};
    bool worker_finished_{false};
    std::mutex join_mutex_;
    std::jthread worker_;
};

}  // namespace ylog
