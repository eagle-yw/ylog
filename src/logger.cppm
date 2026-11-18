export module ylog:logger;

import std;
import :level;
import :message;
import :logger.base;

namespace ylog {

// 同步日志组件：log() 在调用线程直接格式化并扇出到 sink。
// 名称、级别、sink 集合与级别便捷方法都在 detail::LoggerBase 里，这里只留差异。
export class Logger : public detail::LoggerBase<Logger> {
public:
    explicit Logger(std::string name)
        : LoggerBase(std::move(name), Level::Info) {}

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
