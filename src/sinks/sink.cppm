export module ylog:sink;

import std;
import :message;
import :formatter;

namespace ylog {

// Sink 概念：底层输出器只需写字符串。格式化由 AnySink 持有的 formatter 负责。
export template <typename T>
concept Sink = requires(T& sink, std::string_view line) {
    { sink.write(line) } -> std::same_as<void>;
};

// 类型擦除 sink，持有独立 formatter。
// 每个 AnySink 可有不同 pattern（spdlog 的"formatter 归属 sink"设计）。
export class AnySink {
public:
    template <Sink T>
    explicit AnySink(T&& sink, std::unique_ptr<Formatter> formatter = std::make_unique<PatternFormatter>())
        : model_(std::make_unique<Model<std::decay_t<T>>>(std::forward<T>(sink))),
          formatter_(std::move(formatter)) {}

    AnySink(const AnySink&) = delete;
    auto operator=(const AnySink&) -> AnySink& = delete;
    AnySink(AnySink&&) noexcept = default;
    auto operator=(AnySink&&) noexcept -> AnySink& = default;
    ~AnySink() = default;

    // 格式化消息后交给底层 sink 输出。buffer 复用，避免每条日志分配。
    auto write(const Message& message) -> void {
        buffer_.clear();
        formatter_->format(message, buffer_);
        model_->write(buffer_);
    }

    auto flush() -> void { model_->flush(); }

    auto set_pattern(std::string_view pattern) -> void {
        formatter_->set_pattern(pattern);
    }

    auto set_formatter(std::unique_ptr<Formatter> formatter) -> void {
        formatter_ = std::move(formatter);
    }

    auto pattern() const -> std::string_view { return formatter_->pattern(); }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual auto write(std::string_view) -> void = 0;
        virtual auto flush() -> void = 0;
    };

    template <Sink T>
    struct Model final : Concept {
        explicit Model(T sink) : sink_(std::move(sink)) {}
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
    std::string buffer_;
};

} // namespace ylog
