export module ylog:logger;

import std;
import :level;
import :message;
import :sink;
import :logger.base;

namespace ylog {

// 创建期输入：sink 集合在这里定型，之后只可逐 sink 改 pattern，不可增删。
// 集合为空是合法状态，语义等同于丢弃（与 NullSink 一致）。
export struct LoggerConfig {
    std::string name{};
    Level level = Level::Info;
    std::vector<AnySink> sinks{};  // 默认空：零 sink 合法，语义是丢弃
};

// 同步日志组件：log() 在调用线程直接格式化并扇出到 sink。
// 名称、级别、sink 集合与级别便捷方法都在 detail::LoggerBase 里，这里只留差异。
export class Logger : public detail::LoggerBase<Logger> {
public:
    explicit Logger(LoggerConfig config)
        : LoggerBase(std::move(config.name), config.level, std::move(config.sinks)) {}

    auto flush() -> void { sink_set().flush(); }

    // 核心日志入口。SourceLocation 默认参数在调用点捕获位置。
    // Logger 只负责生成 Message，格式化由各 sink 自己的 formatter 完成。
    template <typename... Args>
    auto log(SourceLocation location,
             Level level,
             std::format_string<Args...> format,
             Args&&... args) -> void {
        if (!should_log(level)) {
            return;
        }
        auto user_message = std::format(format, std::forward<Args>(args)...);
        auto message = Message(level, name(), location, user_message);
        sink_set().write(message);
        // flush_on：同步路径在写出后立刻落盘；flush 失败照常向调用方抛出。
        if (level >= flush_level()) {
            flush();
        }
    }
};

}  // namespace ylog
