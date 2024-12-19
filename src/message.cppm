
export module ylog:message;


import std;
import :level;

namespace ylog {



static std::array<std::string, 4> level_strs = {
    "debug",
    "info",
    "warning",
    "error"
};

// Format with source location
struct FmtLoc {
    FmtLoc(
        const char* fmt, 
        const std::source_location& loc = std::source_location::current()
    ): fmt(fmt), loc(loc){}
    
    const char* fmt;
    const std::source_location& loc;    
};

class Message {
public:
    Message(Level level, FmtLoc fmt_loc, std::string_view logger_name, std::string&& msg)
    : fmt_loc_(fmt_loc){
        
        this->logger_name_ = logger_name;
        this->level_ = level;
        this->msg_ = msg;
        this->time_ = std::chrono::system_clock::now();
    }

    std::string format() const {
        auto p = std::filesystem::path(fmt_loc_.loc.file_name());
        auto filename = p.filename().string();
        auto str = std::format("{} [{}] [{}:{}] {}\n",
            str_time(), 
            str_level(), 
            filename, 
            fmt_loc_.loc.line(), 
            msg_
        );
        return str;
    }

private:
    std::string str_time() const{
        auto epoch  = time_.time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count() % 1000;
        std::time_t time_t_value = std::chrono::system_clock::to_time_t(time_);
        std::tm local_time = *std::localtime(&time_t_value);
        std::ostringstream oss;
        oss << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
  
        return std::format("{}.{:03}", oss.str(), ms);            
    }

    std::string_view str_level() const{
        return level_strs[static_cast<int>(level_)];
    }
    
    
    std::chrono::system_clock::time_point time_;
    std::string_view logger_name_;
    std::string msg_;
    
    Level level_;
    FmtLoc fmt_loc_;
};

}