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
    Block,
};

namespace detail {

// 默认上报：写 stderr 并换行。限频在后续工单处理，这里保持最简实现。
inline auto default_error_handler(std::string_view text) -> void {
    std::println(std::cerr, "ylog: {}", text);
}

}  // namespace detail

export struct AsyncLoggerConfig {
    std::string name;
    Level level = Level::Info;
    std::size_t queue_capacity = 8192;
    OverflowPolicy overflow_policy = OverflowPolicy::Block;
    ErrorHandler err_handler = detail::default_error_handler;
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
        sinks_.emplace_back(StdoutSink{});
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
    }

    AsyncLogger(const AsyncLogger&) = delete;
    auto operator=(const AsyncLogger&) -> AsyncLogger& = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    auto operator=(AsyncLogger&&) -> AsyncLogger& = delete;

    // 先请求停止，再由工作线程按 FIFO 排空队列，最后 join。
    ~AsyncLogger() {
        worker_.request_stop();
        cv_ready_.notify_all();
        cv_space_.notify_all();
        worker_.join();
    }

    auto name() const -> std::string_view { return name_; }
    auto level() const -> Level { return level_.load(std::memory_order_relaxed); }
    auto set_level(Level target) -> void { level_.store(target, std::memory_order_relaxed); }

    auto queue_capacity() const -> std::size_t { return queue_capacity_; }
    auto overflow_policy() const -> OverflowPolicy { return overflow_policy_; }

    // 追加 sink，可为其单独指定 pattern（formatter 归属 sink）。
    template <Sink T>
    auto append_sink(T&& sink, std::string_view pattern = default_pattern) -> void {
        sinks_.emplace_back(std::forward<T>(sink), std::make_unique<PatternFormatter>(pattern));
    }

    // 追加 sink 并注入配置好的 formatter（可带自定义 flag）。
    template <Sink T>
    auto append_sink(T&& sink, std::unique_ptr<Formatter> formatter) -> void {
        sinks_.emplace_back(std::forward<T>(sink), std::move(formatter));
    }

    // 为所有 sink 设置 pattern（扇出）。
    auto set_pattern(std::string_view pattern) -> void {
        for (auto& sink : sinks_) {
            sink.set_pattern(pattern);
        }
    }

    // 屏障：返回时先前入队的消息都已 write，且所有 sink 已 flush。
    auto flush() -> void {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        {
            std::unique_lock lock(mutex_);
            if (worker_finished_) {
                lock.unlock();
                for (auto& sink : sinks_) {
                    sink.flush();
                }
                return;
            }
            queue_.emplace_back(FlushCommand{std::move(done)});
        }
        cv_ready_.notify_one();
        future.wait();
    }

    // 核心日志入口。SourceLocation 默认参数在调用点捕获位置。
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
        {
            std::unique_lock lock(mutex_);
            cv_space_.wait(lock,
                           [this] { return worker_finished_ || queue_.size() < queue_capacity_; });
            if (worker_finished_) {
                return;
            }
            queue_.emplace_back(std::move(message));
        }
        cv_ready_.notify_one();
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
    struct FlushCommand {
        std::shared_ptr<std::promise<void>> done;
    };

    using QueueItem = std::variant<Message, FlushCommand>;

    auto should_log(Level msg_level) const -> bool {
        return msg_level >= level_.load(std::memory_order_relaxed);
    }

    auto validate_config() const -> void {
        if (queue_capacity_ == 0) {
            ylog_detail_errors::fail_config<std::invalid_argument>("queue_capacity 必须大于 0");
        }
        switch (overflow_policy_) {
            case OverflowPolicy::Block:
                break;
            default:
                ylog_detail_errors::fail_config<std::invalid_argument>("未知的 overflow_policy");
        }
    }

    // 工作线程：停止请求后先把队列排空，再退出。
    auto run(std::stop_token token) -> void {
        for (;;) {
            std::unique_lock lock(mutex_);
            cv_ready_.wait(lock, token, [this] { return !queue_.empty(); });
            if (queue_.empty()) {
                if (token.stop_requested()) {
                    worker_finished_ = true;
                    return;
                }
                continue;
            }
            auto item = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            cv_space_.notify_one();

            if (auto* message = std::get_if<Message>(&item)) {
                for (auto& sink : sinks_) {
                    sink.write(*message);
                }
            } else {
                for (auto& sink : sinks_) {
                    sink.flush();
                }
                std::get<FlushCommand>(item).done->set_value();
            }
        }
    }

    std::string name_;
    std::atomic<Level> level_{Level::Info};
    std::size_t queue_capacity_{8192};
    OverflowPolicy overflow_policy_{OverflowPolicy::Block};
    ErrorHandler err_handler_;

    std::vector<AnySink> sinks_;
    std::mutex mutex_;
    std::condition_variable_any cv_ready_;
    std::condition_variable_any cv_space_;
    std::deque<QueueItem> queue_;
    bool worker_finished_{false};
    std::jthread worker_;
};

}  // namespace ylog
