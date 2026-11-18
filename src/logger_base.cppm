module ylog:logger.base;

import std;
import :level;
import :message;
import :formatter;
import :sink;
import ylog_detail_errors;

namespace ylog {

namespace detail {

// Logger 与 AsyncLogger 的公共面：名称、级别、创建期定型的 sink 集合与六个级别便捷方法。
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

    // 调用方的守门函数：跳过昂贵的参数构造与 std::format。
    auto should_log(Level msg_level) const -> bool {
        return msg_level >= level_.load(std::memory_order_relaxed);
    }

    // sink 集合视图：结构在创建期定型、元素可配置（逐 sink 的 set_pattern / set_formatter）。
    auto sinks() -> std::span<AnySink> { return sinks_.sinks(); }

    // flush 阈值：达到该级别的消息写出后主动 flush sink，默认 Off 表示从不触发。
    // 与 set_level 同属运行期可改的原子配置，热路径只做一次 relaxed 读。
    auto flush_on(Level target) -> void { flush_level_.store(target, std::memory_order_relaxed); }
    auto flush_level() const -> Level { return flush_level_.load(std::memory_order_relaxed); }

    // 为所有 sink 设置 pattern（扇出）。
    auto set_pattern(std::string_view pattern) -> void { sinks_.set_pattern(pattern); }

    // 给每个 sink 注入同一份 formatter 配置：逐 sink clone，之后各自独立演进。
    // 空入参先判：这是调用方违约，在唯一入口处拒绝。
    auto set_formatter(std::unique_ptr<Formatter> formatter) -> void {
        if (!formatter) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                "set_formatter 需要一个非空的 formatter");
        }
        sinks_.set_formatter(*formatter);
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
    // sink 集合在创建期定型：构造器之后再无增删接口，工作线程与配置期因此不必互斥。
    LoggerBase(std::string name, Level level, std::vector<AnySink> sinks)
        : name_(std::move(name)),
          level_(level),
          sinks_(std::move(sinks)) {}

    // 写入路径的入口：同步直写，或异步带失败回调。
    auto sink_set() -> SinkSet& { return sinks_; }

private:
    auto self() -> Derived& { return static_cast<Derived&>(*this); }

    std::string name_;
    std::atomic<Level> level_;
    std::atomic<Level> flush_level_{Level::Off};
    SinkSet sinks_;
};

}  // namespace detail

}  // namespace ylog
