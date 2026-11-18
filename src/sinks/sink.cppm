export module ylog:sink;

import std;
import :message;
import :formatter;

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

}  // namespace ylog
