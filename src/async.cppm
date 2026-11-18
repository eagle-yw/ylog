module;

// assert 是宏，`import std;` 带不进来；内部不变量要用它，只能在全局模块片段里取。
#include <cassert>

export module ylog:async;

import std;
import :level;
import :message;
import :logger.base;
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
// 契约：不实现 Sink concept，也不修改 Logger；名称/级别/sink 集合与级别便捷方法
// 来自 detail::LoggerBase，这里只追加队列与工作线程。
//
// sink 集合只在配置期可变：worker 在构造末尾就已启动，启动后追加 sink 会与它的遍历
// 竞争。首个入队或 stop() 之后 append_sink/set_pattern 触发断言（调试构建）。
// err_handler 在 worker 线程（sink 失败）与调用线程（丢弃/停止）上同步调用：worker 上
// 的 handler 不得重入本组件的 log()/flush()/stop()，否则等的是自己。见 ADR-0003。
export class AsyncLogger : public detail::LoggerBase<AsyncLogger> {
public:
    explicit AsyncLogger(AsyncLoggerConfig config)
        : LoggerBase(std::move(config.name), config.level),
          queue_capacity_(config.queue_capacity),
          overflow_policy_(config.overflow_policy),
          err_handler_(std::move(config.err_handler)) {
        validate_config();
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
            seal_configuration();
        }
        worker_.request_stop();
        cv_ready_.notify_all();
        cv_space_.notify_all();
        std::lock_guard lock(join_mutex_);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    auto queue_capacity() const -> std::size_t { return queue_capacity_; }
    auto overflow_policy() const -> OverflowPolicy { return overflow_policy_; }

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
        // 文本快照在任何 move 之前取：上报不依赖「这个分支恰好没有 move」这一隐式事实。
        auto text = message.formatted();
        auto outcome = EnqueueOutcome{};
        {
            std::unique_lock lock(mutex_);
            if (stopping_) {
                outcome.note(EnqueueOutcome::Loss::Rejected, text);
            } else {
                switch (overflow_policy_) {
                    case OverflowPolicy::Block:
                        // 停止请求也要唤醒等待容量的生产者：让它放弃入队，而不是等到
                        // 队列腾出空位——排空期间队列可能永远不空。
                        cv_space_.wait(
                            lock, [this] { return stopping_ || queue_.size() < queue_capacity_; });
                        if (stopping_) {
                            outcome.note(EnqueueOutcome::Loss::Rejected, text);
                        } else {
                            enqueue(std::move(message), outcome);
                        }
                        break;
                    case OverflowPolicy::OverrunOldest:
                        if (queue_.size() >= queue_capacity_) {
                            outcome.note(EnqueueOutcome::Loss::Overrun, evict_oldest_locked());
                        }
                        enqueue(std::move(message), outcome);
                        break;
                    case OverflowPolicy::DiscardNew:
                        if (queue_.size() >= queue_capacity_) {
                            outcome.note(EnqueueOutcome::Loss::Discarded, text);
                        } else {
                            enqueue(std::move(message), outcome);
                        }
                        break;
                }
            }
        }
        if (outcome.enqueued) {
            cv_ready_.notify_one();
        }
        outcome.report(err_handler_);
    }

private:
    // 一次入队的结果。三种丢失互斥，共用「为什么丢 + 丢的是哪条」，
    // 上报因此收在一次调用里，log() 尾部不再三个 if 并列。
    struct EnqueueOutcome {
        enum class Loss { None, Overrun, Discarded, Rejected };

        Loss loss = Loss::None;
        std::string lost;  // 丢失条目的文本快照；入队成功时为空
        bool enqueued = false;

        // 文本在 Message 被 move 之前拷进来，避免持有指向已搬走对象的视图。
        auto note(Loss kind, std::string_view text) -> void {
            loss = kind;
            lost = text;
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

    // 队列里只有消息：序号在入队时分配，flush() 用它表达「刷到哪一条为止」。
    struct QueuedMessage {
        Message message;
        std::size_t sequence;
    };

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

    // 入队成功只在这一个地方记录，避免调用点各写一次布尔量。
    // 调用方持有 mutex_：这里也是配置期结束的唯一起点（worker 从此可能遍历 sink）。
    auto enqueue(Message message, EnqueueOutcome& outcome) -> void {
        auto reached_flush_level = message.level() >= flush_level();
        queue_.push_back(QueuedMessage{std::move(message), next_sequence_++});
        outcome.enqueued = true;
        if (reached_flush_level) {
            // flush_on 折算成一次序号屏障请求：生产者不等落盘，也不占队列槽位，
            // 与 flush() 走同一套机制，因此不会改变它的屏障语义。
            flush_target_ = std::max(flush_target_, next_sequence_);
        }
        seal_configuration();
        // 不变量：队列里只有消息，长度受容量约束，flush 不占用队列空间。
        assert(queue_.size() <= queue_capacity_);
    }

    // 调用方必须持有 mutex_。摘掉最旧的一条（OverrunOldest），返回它的文本供上报。
    auto evict_oldest_locked() -> std::string {
        auto text = std::string(queue_.front().message.formatted());
        queue_.pop_front();
        return text;
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

    // 工作线程：投递队列，按需为目标序号落盘；停止请求后先把队列排空，再退出。
    auto run(std::stop_token token) -> void {
        for (;;) {
            std::unique_lock lock(mutex_);
            cv_ready_.wait(lock, token,
                           [this] { return !queue_.empty() || flush_target_ > flushed_sequence_; });

            // flush 请求按序号结算，而不是绑在「队列排空」上：目标之内的消息都写出后
            // 立刻落盘，flush_on 因此能在那一条之后落盘，flush() 也能更早返回。
            if (flush_target_ > flushed_sequence_ && processed_sequence_ >= flush_target_) {
                auto target = flush_target_;
                lock.unlock();
                flush_sinks();
                lock.lock();
                flushed_sequence_ = std::max(flushed_sequence_, target);
                cv_flushed_.notify_all();
                continue;
            }
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
            // 走到这里只剩「已收到停止请求」一种可能：目标序号总是在入队成功后取
            // 刚分配的那个序号，而队列按 FIFO 写、覆盖策略只删队首，所以目标之内的
            // 序号要么被写出、要么被更晚的写出顺带推过，processed_sequence_ 必定追上它。
            assert(flush_target_ <= flushed_sequence_ || token.stop_requested());
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

    std::size_t queue_capacity_{8192};
    OverflowPolicy overflow_policy_{OverflowPolicy::Block};
    ErrorHandler err_handler_;

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
