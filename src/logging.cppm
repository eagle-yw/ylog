export module ylog:logging;

import std;
import :level;
import :logger;
import :registry;

namespace ylog {

// 默认 logger 就是 Registry 中名为 default 的常驻槽位，与 create("default") 得到同一个
// 实例（ADR-0010）。槽位与它的初始 sink 集合都由 Registry::default_logger() 建立
// （ADR-0011），本分区只缓存引用：热路径因此无锁，实例归 Registry 持有，且
// drop("default") 被拒绝，所以这个引用不会悬垂。
//
// 分区名不能叫 default：它是关键字，模块分区名只接受标识符。本分区与 :logger 分开，
// 是因为 :registry 依赖 :logger，写进 :logger 会造成分区循环。
export auto default_logger() -> Logger& {
    static Logger& instance = Registry::instance().default_logger();
    return instance;
}

export auto set_level(Level target) -> void { default_logger().set_level(target); }
export auto set_pattern(std::string_view pattern) -> void { default_logger().set_pattern(pattern); }

export template <typename... Args>
auto log(SourceLocation loc, Level level, std::format_string<Args...> fmt, Args&&... args) -> void {
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

}  // namespace ylog
