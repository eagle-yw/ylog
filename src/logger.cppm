export module ylog:logger;

import std;
import :level;
import :message;


namespace ylog {

export class Logger {
public:
    Logger() {
        level_ = Level::Info;
        name_ = "Logger";
        std::println("Logger created");
    }

    void debug(FmtLoc fmt_loc, auto &&...args) {
        log(Level::Debug, fmt_loc, std::forward<decltype(args)>(args)...);
    }

    void info(FmtLoc fmt_loc, auto &&...args) {
        log(Level::Info, fmt_loc, std::forward<decltype(args)>(args)...);
    }

    void warning(FmtLoc fmt_loc, auto &&...args) {
        log(Level::Warning, fmt_loc, std::forward<decltype(args)>(args)...);
    }

    void error(FmtLoc fmt_loc, auto &&...args) {
        log(Level::Error, fmt_loc, std::forward<decltype(args)>(args)...);
    }

    void set_level(const Level level) {
        level_ = level;
    }

private:
    bool level_is_enabled(Level msg_level) {
        return msg_level >= level_.load(std::memory_order_relaxed);
    }

    void log(Level level, FmtLoc loc_fmt, auto &&...args){
        bool log_enabled = level_is_enabled(level);
        if(!log_enabled){
            return;
        }

        auto input_msg = std::string();        
        std::vformat_to(std::back_inserter(input_msg), loc_fmt.fmt, std::make_format_args(args...));
        auto log_msg = Message(level, loc_fmt, name_, std::move(input_msg));

        log_it(log_msg);
    }

    void log_it(Message log_msg) {
        //todo
        std::println("{}",log_msg.format());
    }

    std::atomic<Level> level_;
    std::string name_;

    
};

Logger& getDefaultLogger() {
    static auto default_logger = Logger();
    return default_logger;
}

export void debug(FmtLoc fmt_loc, auto &&...args){
    getDefaultLogger().debug(fmt_loc, std::forward<decltype(args)>(args)...);
}

export void info(FmtLoc fmt_loc, auto &&...args){
    getDefaultLogger().info(fmt_loc, std::forward<decltype(args)>(args)...);
}

export void warning(FmtLoc fmt_loc, auto &&...args){
    getDefaultLogger().warning(fmt_loc, std::forward<decltype(args)>(args)...);
}

export void error(FmtLoc fmt_loc, auto &&...args){
    getDefaultLogger().error(fmt_loc, std::forward<decltype(args)>(args)...);
}

export void set_level(const Level level){
    getDefaultLogger().set_level(level);
}

} // namespace ylog end
