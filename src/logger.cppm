export module ylog:logger;

import std;
import :level;
import :message;
import :formatter;
import :sink;
import :sink.stdout;

namespace ylog {

export class Logger {
public:
    explicit Logger(std::string name)
        : name_(std::move(name)) {
        sinks_.emplace_back(StdoutSink{});
    }

    auto name() const -> std::string_view { return name_; }
    auto level() const -> Level { return level_.load(std::memory_order_relaxed); }
    auto set_level(Level target) -> void { level_.store(target, std::memory_order_relaxed); }

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

    auto flush() -> void {
        for (auto& sink : sinks_) {
            sink.flush();
        }
    }

    // 核心日志入口。SourceLocation 默认参数在调用点捕获位置。
    // Logger 只负责生成 Message，格式化由各 sink 自己的 formatter 完成。
    template <typename... Args>
    auto log(SourceLocation location, Level level,
             std::format_string<Args...> format, Args&&... args) -> void {
        if (!should_log(level)) {
            return;
        }
        auto user_message = std::format(format, std::forward<Args>(args)...);
        auto message = Message(level, name_, location, user_message);
        for (auto& sink : sinks_) {
            sink.write(message);
        }
    }

    // 各级别便捷方法。第一个参数 {} 自动捕获调用点。
    // 结构化日志直接用 reflected：logger.info({}, "{}", reflected(point))。
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
    auto should_log(Level msg_level) const -> bool {
        return msg_level >= level_.load(std::memory_order_relaxed);
    }

    std::string name_;
    std::atomic<Level> level_{Level::Info};
    std::vector<AnySink> sinks_;
};

export auto default_logger() -> Logger& {
    static Logger logger("default");
    return logger;
}

export auto set_level(Level target) -> void { default_logger().set_level(target); }
export auto set_pattern(std::string_view pattern) -> void { default_logger().set_pattern(pattern); }

export template <typename... Args>
auto log(SourceLocation loc, Level level,
         std::format_string<Args...> fmt, Args&&... args) -> void {
    default_logger().log(loc, level, fmt, std::forward<Args>(args)...);
}

export template <typename... Args>
auto trace(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
    default_logger().log(loc, Level::Trace, fmt, std::forward<Args>(args)...);
}
export template <typename... Args>
auto debug(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
    default_logger().log(loc, Level::Debug, fmt, std::forward<Args>(args)...);
}
export template <typename... Args>
auto info(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
    default_logger().log(loc, Level::Info, fmt, std::forward<Args>(args)...);
}
export template <typename... Args>
auto warning(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
    default_logger().log(loc, Level::Warning, fmt, std::forward<Args>(args)...);
}
export template <typename... Args>
auto error(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
    default_logger().log(loc, Level::Error, fmt, std::forward<Args>(args)...);
}
export template <typename... Args>
auto critical(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
    default_logger().log(loc, Level::Critical, fmt, std::forward<Args>(args)...);
}

} // namespace ylog
