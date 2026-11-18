export module ylog:sink;

import std;
import :message;
import :formatter;
import ylog_detail_errors;

namespace ylog {

// Sink 概念：底层输出器只需写字符串。格式化由 AnySink 持有的 formatter 负责。
// 契约：line 是借用视图，只在 write 调用期间有效，实现不得保存。
export template <typename T>
concept Sink = requires(T& sink, std::string_view line) {
    { sink.write(line) } -> std::same_as<void>;
};

namespace detail {

// 每条日志落到本线程的暂存缓冲：格式化在输出锁之外完成，无法复用 sink 内的
// 共享缓冲；按线程复用，避免每条日志一次分配。
// 注意：自定义 flag 处理器不得在同一条消息的格式化过程中再次记录日志，
// 否则会覆盖本缓冲（重入不受支持）。
inline auto scratch_buffer() -> std::string& {
    thread_local std::string buffer;
    return buffer;
}

}  // namespace detail

// 类型擦除 sink，持有独立 formatter。
// 每个 AnySink 可有不同 pattern（spdlog 的"formatter 归属 sink"设计）。
//
// 并发契约（两级锁，从不嵌套）：
// - formatter_mutex_ 保护 formatter 实例与配置变更（set_pattern / set_formatter）；
// - output_mutex_    保护底层 sink 的输出设备（write / flush）。
// write() 先在 formatter_mutex_ 下把消息格式化到线程局部缓冲，释放后再取
// output_mutex_ 输出；因此某线程在等 I/O 时，其他线程仍可并行格式化。
// 格式化本身仍是串行的：PatternFormatter 缓存 tm，且自定义 flag 处理器按契约
// 有状态（format 非 const），同一个 formatter 实例不能被多线程同时 format。
export class AnySink {
public:
    template <Sink T>
    explicit AnySink(T&& sink,
                     std::unique_ptr<Formatter> formatter = std::make_unique<PatternFormatter>())
        : model_(std::make_unique<Model<std::decay_t<T>>>(std::forward<T>(sink))),
          formatter_(std::move(formatter)) {}

    AnySink(const AnySink&) = delete;
    auto operator=(const AnySink&) -> AnySink& = delete;

    // 移动只在配置期发生（Logger 的 sinks_ 扩容），锁不参与移动。
    AnySink(AnySink&& other) noexcept
        : model_(std::move(other.model_)),
          formatter_(std::move(other.formatter_)) {}

    auto operator=(AnySink&& other) noexcept -> AnySink& {
        if (this != &other) {
            model_ = std::move(other.model_);
            formatter_ = std::move(other.formatter_);
        }
        return *this;
    }

    ~AnySink() = default;

    // 格式化在输出临界区之外：先写线程局部缓冲，再短暂持锁交给底层 sink。
    auto write(const Message& message) -> void {
        auto& scratch = detail::scratch_buffer();
        scratch.clear();
        {
            std::lock_guard lock(formatter_mutex_);
            formatter_->format(message, scratch);
        }
        {
            std::lock_guard lock(output_mutex_);
            model_->write(scratch);
        }
    }

    auto flush() -> void {
        std::lock_guard lock(output_mutex_);
        model_->flush();
    }

    auto set_pattern(std::string_view pattern) -> void {
        std::lock_guard lock(formatter_mutex_);
        formatter_->set_pattern(pattern);
    }

    auto set_formatter(std::unique_ptr<Formatter> formatter) -> void {
        std::lock_guard lock(formatter_mutex_);
        formatter_ = std::move(formatter);
    }

    // 返回的视图在下一次 set_pattern / set_formatter 之前有效。
    auto pattern() const -> std::string_view {
        std::lock_guard lock(formatter_mutex_);
        return formatter_->pattern();
    }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual auto write(std::string_view) -> void = 0;
        virtual auto flush() -> void = 0;
    };

    template <Sink T>
    struct Model final : Concept {
        explicit Model(T sink)
            : sink_(std::move(sink)) {}
        auto write(std::string_view line) -> void override { sink_.write(line); }
        auto flush() -> void override {
            if constexpr (requires { sink_.flush(); }) {
                sink_.flush();
            }
        }
        T sink_;
    };

    std::unique_ptr<Concept> model_;
    std::unique_ptr<Formatter> formatter_;
    mutable std::mutex formatter_mutex_;
    std::mutex output_mutex_;
};

namespace detail {

// sink 调用守卫：把异常收敛成一次回调。
// 必须是模板：-fno-exceptions 下 try/catch 的语义检查发生在实例化期，
// 被 if constexpr 丢弃的分支才不会报「exception handling disabled」。
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

// sink 扇出集合：Logger 与 AsyncLogger 共用。
//
// 两级锁与 formatter 归属都在 AnySink 里（见 ADR-0004），这里只负责「按顺序对每个
// sink 做一次动作」。同步路径让异常照常外传；异步路径用带回调的重载把失败收敛成上报。
class SinkSet {
public:
    // 追加 sink，可为其单独指定 pattern（formatter 归属 sink）。
    template <Sink T>
    auto append(T&& sink, std::string_view pattern) -> void {
        slots_.push_back(
            Slot{AnySink(std::forward<T>(sink), std::make_unique<PatternFormatter>(pattern))});
    }

    // 追加 sink 并注入配置好的 formatter（可带自定义 flag）。
    template <Sink T>
    auto append(T&& sink, std::unique_ptr<Formatter> formatter) -> void {
        slots_.push_back(Slot{AnySink(std::forward<T>(sink), std::move(formatter))});
    }

    // 为所有 sink 设置 pattern（扇出）。
    auto set_pattern(std::string_view pattern) -> void {
        for (auto& slot : slots_) {
            slot.sink.set_pattern(pattern);
        }
    }

    // 同步路径：异常照常向外传播，行为与内联循环一致。
    auto write(const Message& message) -> void {
        for (auto& slot : slots_) {
            slot.sink.write(message);
        }
    }

    // 异步路径：单个 sink 失败只回调一次并跳过，不影响其它 sink。
    // 连续失败只上报一次，该 sink 下一次写成功即复位。
    template <typename OnFailure>
    auto write(const Message& message, OnFailure&& on_failure) -> void {
        for (auto& slot : slots_) {
            auto failure = std::string{};
            guard_sink_call([&slot, &message] { slot.sink.write(message); },
                            [&failure](std::string_view why) { failure = std::string(why); });
            if (failure.empty()) {
                slot.failing = false;
                continue;
            }
            if (slot.failing) {
                continue;
            }
            slot.failing = true;
            std::invoke(on_failure, std::string_view(failure));
        }
    }

    auto flush() -> void {
        for (auto& slot : slots_) {
            slot.sink.flush();
        }
    }

    // flush 失败每次都回调：只有显式 flush 才走到这里，频率由调用方控制。
    template <typename OnFailure>
    auto flush(OnFailure&& on_failure) -> void {
        for (auto& slot : slots_) {
            guard_sink_call([&slot] { slot.sink.flush(); }, on_failure);
        }
    }

private:
    // sink 与它的失败状态放在一起：连续失败只在进入失败状态时上报一次。
    struct Slot {
        AnySink sink;
        bool failing{false};
    };

    std::vector<Slot> slots_;
};

}  // namespace detail

}  // namespace ylog
