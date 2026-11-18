export module ylog:sink;

import std;
import :level;
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

// 颜色是 sink 的配置而不是 sink 的种类。上色动作由 AnySink 执行（只有 formatter 知道
// 颜色区间，sink 只拿到一行字符串），sink 侧用可选协议 color_mode() 声明自己的意愿。
export enum class ColorMode {
    Never,
    Automatic,
    Always,
};

// 丢弃所有行。无配置、无状态：用于基准测试，或临时关掉某个目的地。
// 不提供 flush()，顺带验证 Sink 协议的可选部分。
export class NullSink {
public:
    auto write(std::string_view) -> void {}
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

// 级别 → 前景色。区间本身由 `%^`/`%$` 决定，级别只决定用什么颜色包住它。
inline auto ansi_prefix(Level level) -> std::string_view {
    switch (level) {
        case Level::Trace:
            return "\x1b[90m";
        case Level::Debug:
            return "\x1b[36m";
        case Level::Info:
            return "\x1b[32m";
        case Level::Warning:
            return "\x1b[33m";
        case Level::Error:
            return "\x1b[31m";
        case Level::Critical:
            return "\x1b[1;31m";
        case Level::Off:
            break;
    }
    return {};
}

inline constexpr std::string_view ansi_reset{"\x1b[0m"};

// 在 [start, end) 两端插入转义序列。区间倒置或越界时宁可不写字节，也不产出半截转义。
// 就地插入：整行已经在 scratch 里，不需要为它再复制一份。
inline auto apply_color(std::string& line, Level level, std::pair<std::size_t, std::size_t> range)
    -> void {
    auto [start, end] = range;
    auto prefix = ansi_prefix(level);
    if (prefix.empty() || start >= end || end > line.size()) {
        return;
    }
    line.insert(end, ansi_reset);
    line.insert(start, prefix);
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
        auto range = std::pair<std::size_t, std::size_t>{};
        scratch.clear();
        {
            std::lock_guard lock(formatter_mutex_);
            formatter_->format(message, scratch);
            // 区间是 formatter 的每次格式化状态，必须在同一把锁下取走。
            range = formatter_->color_range();
        }
        {
            std::lock_guard lock(output_mutex_);
            // 上色在这里：formatter 只交出区间，sink 只收一行字符串（ADR-0009）。
            if (model_->color_mode() != ColorMode::Never) {
                detail::apply_color(scratch, message.level(), range);
            }
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

    // 空 formatter 会让 write / pattern 空解引用，因此在唯一入口处就拒绝。
    auto set_formatter(std::unique_ptr<Formatter> formatter) -> void {
        if (!formatter) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                "set_formatter 需要一个非空的 formatter");
        }
        std::lock_guard lock(formatter_mutex_);
        formatter_ = std::move(formatter);
    }

    // 返回的视图在下一次 set_pattern / set_formatter 之前有效。
    auto pattern() const -> std::string_view {
        std::lock_guard lock(formatter_mutex_);
        return formatter_->pattern();
    }

    // 底层 sink 声明的颜色模式；没有声明该协议的 sink 视为永不着色。
    auto color_mode() const -> ColorMode { return model_->color_mode(); }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual auto write(std::string_view) -> void = 0;
        virtual auto flush() -> void = 0;
        virtual auto color_mode() const -> ColorMode = 0;
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
        auto color_mode() const -> ColorMode override {
            if constexpr (requires { sink_.color_mode(); }) {
                return sink_.color_mode();
            } else {
                return ColorMode::Never;
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

// sink 扇出集合：Logger 与 AsyncLogger 共用。
//
// 两级锁与 formatter 归属都在 AnySink 里（见 ADR-0004），这里只负责「按顺序对每个
// sink 做一次动作」。同步路径让异常照常外传；异步路径用带回调的重载把失败收敛成上报。
//
// 存储是两个等长向量：sinks_ 必须是 AnySink 的连续数组（sinks() 把它直接交出去），
// 所以失败状态不能和它挤在同一个元素里。两者只在 push() 里同时增长。
class SinkSet {
public:
    // 追加 sink，可为其单独指定 pattern（formatter 归属 sink）。
    template <Sink T>
    auto append(T&& sink, std::string_view pattern) -> void {
        push(AnySink(std::forward<T>(sink), std::make_unique<PatternFormatter>(pattern)));
    }

    // 追加 sink 并注入配置好的 formatter（可带自定义 flag）。
    template <Sink T>
    auto append(T&& sink, std::unique_ptr<Formatter> formatter) -> void {
        push(AnySink(std::forward<T>(sink), std::move(formatter)));
    }

    // 结构不可变、元素可配置。之后的 append 会让底层数组重新分配，视图随即失效。
    auto sinks() -> std::span<AnySink> { return sinks_; }

    // 为所有 sink 设置 pattern（扇出）。
    auto set_pattern(std::string_view pattern) -> void {
        for (auto& sink : sinks_) {
            sink.set_pattern(pattern);
        }
    }

    // formatter 归属 sink：每个 sink 拿独立 clone，改一个不影响另一个。
    auto set_formatter(const Formatter& prototype) -> void {
        for (auto& sink : sinks_) {
            sink.set_formatter(prototype.clone());
        }
    }

    // 同步路径：异常照常向外传播，行为与内联循环一致。
    auto write(const Message& message) -> void {
        for (auto& sink : sinks_) {
            sink.write(message);
        }
    }

    // 异步路径：单个 sink 失败只回调一次并跳过，不影响其它 sink。
    // 连续失败只上报一次，该 sink 下一次写成功即复位。
    template <typename OnFailure>
    auto write(const Message& message, OnFailure&& on_failure) -> void {
        for (auto index = std::size_t{0}; index < sinks_.size(); ++index) {
            auto failure = std::string{};
            ylog_detail_errors::guard_call(
                [this, index, &message] { sinks_[index].write(message); },
                [&failure](std::string_view why) { failure = std::string(why); });
            if (failure.empty()) {
                failing_[index] = false;
                continue;
            }
            if (failing_[index]) {
                continue;
            }
            failing_[index] = true;
            std::invoke(on_failure, std::string_view(failure));
        }
    }

    auto flush() -> void {
        for (auto& sink : sinks_) {
            sink.flush();
        }
    }

    // flush 失败每次都回调：只有显式 flush 才走到这里，频率由调用方控制。
    template <typename OnFailure>
    auto flush(OnFailure&& on_failure) -> void {
        for (auto& sink : sinks_) {
            ylog_detail_errors::guard_call([&sink] { sink.flush(); }, on_failure);
        }
    }

private:
    // sink 与它的失败状态必须同时增长，因此只留这一个入口。
    auto push(AnySink sink) -> void {
        sinks_.push_back(std::move(sink));
        failing_.push_back(false);
    }

    std::vector<AnySink> sinks_;
    std::vector<bool> failing_;  // 与 sinks_ 等长：连续失败只在进入失败状态时上报一次
};

}  // namespace detail

}  // namespace ylog
