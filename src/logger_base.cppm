module;

// assert 是宏，`import std;` 带不进来。
#include <cassert>

module ylog:logger.base;

import std;
import :level;
import :message;
import :formatter;
import :sink;
import :sink.stdout;

namespace ylog {

namespace detail {

// Logger 与 AsyncLogger 的公共面：名称、级别、sink 集合与六个级别便捷方法。
// 派生类只保留差异部分（同步直写 / 异步入队），公开继承让这些成员直接长在派生类上。
//
// 单独占一个分区，:async 因此不必为拿它而 import :logger；ylog 模块的公开面不
// export 本分区，调用方看得见这些成员却无法命名基类。取舍见 ADR-0008。
template <typename Derived>
class LoggerBase {
public:
    auto name() const -> std::string_view { return name_; }
    auto level() const -> Level { return level_.load(std::memory_order_relaxed); }
    auto set_level(Level target) -> void { level_.store(target, std::memory_order_relaxed); }

    // 追加 sink，可为其单独指定 pattern（formatter 归属 sink）。
    template <Sink T>
    auto append_sink(T&& sink, std::string_view pattern = default_pattern) -> void {
        ensure_configurable();
        sinks_.append(std::forward<T>(sink), pattern);
    }

    // 追加 sink 并注入配置好的 formatter（可带自定义 flag）。
    template <Sink T>
    auto append_sink(T&& sink, std::unique_ptr<Formatter> formatter) -> void {
        ensure_configurable();
        sinks_.append(std::forward<T>(sink), std::move(formatter));
    }

    // 为所有 sink 设置 pattern（扇出）。
    auto set_pattern(std::string_view pattern) -> void {
        ensure_configurable();
        sinks_.set_pattern(pattern);
    }

    // 六个级别便捷方法的形状完全一致，只有级别不同：CRTP 调回派生类的 log()。
    template <typename... Args>
    auto trace(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        self().log(loc, Level::Trace, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    auto debug(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        self().log(loc, Level::Debug, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    auto info(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        self().log(loc, Level::Info, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    auto warning(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        self().log(loc, Level::Warning, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    auto error(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        self().log(loc, Level::Error, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    auto critical(SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) -> void {
        self().log(loc, Level::Critical, fmt, std::forward<Args>(args)...);
    }

protected:
    explicit LoggerBase(std::string name, Level level)
        : name_(std::move(name)),
          level_(level) {
        sinks_.append(StdoutSink{}, default_pattern);
    }

    auto should_log(Level msg_level) const -> bool {
        return msg_level >= level_.load(std::memory_order_relaxed);
    }

    // 写入路径的入口：同步直写，或异步带失败回调。
    auto sinks() -> SinkSet& { return sinks_; }

    // 配置期结束。之后 append_sink / set_pattern 属于调用方违约。
    // 断言只是侦查手段，不是同步手段：真正违反前置条件的调用本身就是数据竞争。
    auto seal_configuration() -> void { configurable_ = false; }

private:
    auto self() -> Derived& { return static_cast<Derived&>(*this); }

    // 同步组件没有并发读者，配置随时可改（从不解封）；异步组件在首个入队后封板。
    auto ensure_configurable() const -> void {
        assert(configurable_ && "sink 配置只在配置期可变，异步组件启动后不得变更");
    }

    std::string name_;
    std::atomic<Level> level_;
    SinkSet sinks_;
    bool configurable_ = true;
};

}  // namespace detail

}  // namespace ylog
