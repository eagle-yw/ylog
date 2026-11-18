export module ylog:formatter;

import std;
import :level;
import :message;
import ylog_detail_errors;

namespace ylog {

export inline constexpr std::string_view default_pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%@] %v";

// 自定义 pattern flag 的公开契约：可复制、非常量 format、固定三参数。
export template <typename T>
concept PatternFlag =
    std::copy_constructible<T> &&
    requires(T& handler, const Message& message, const std::tm& time, std::string& dest) {
        { handler.format(message, time, dest) } -> std::same_as<void>;
    } &&
    (!requires(const T& handler, const Message& message, const std::tm& time, std::string& dest) {
        handler.format(message, time, dest);
    });

namespace detail {

// std::localtime 使用静态缓冲区，需要全局加锁。
inline auto localtime_threadsafe(std::time_t seconds) -> std::tm {
    static std::mutex time_mutex;
    std::lock_guard lock(time_mutex);
    std::tm result{};
    if (auto* value = std::localtime(&seconds); value != nullptr) {
        result = *value;
    }
    return result;
}

// 对齐/宽度信息，解析 pattern 时填充。
struct padding_info {
    enum class side { right, left, center };

    std::size_t width{0};
    side align{side::right};

    auto enabled() const -> bool { return width != 0; }
};

// RAII：在析构时按 padding 信息补齐空格。
// 记录写入起点，若内容宽度不足则补空格（左对齐补尾，否则补头）。
class scoped_padder {
public:
    scoped_padder(std::string& dest, padding_info info)
        : dest_(dest), start_(dest.size()), info_(info) {}

    scoped_padder(const scoped_padder&) = delete;
    auto operator=(const scoped_padder&) -> scoped_padder& = delete;

    ~scoped_padder() {
        if (!info_.enabled()) {
            return;
        }
        auto content_size = dest_.size() - start_;
        if (content_size >= info_.width) {
            return;
        }
        auto fill = info_.width - content_size;
        if (info_.align == padding_info::side::left) {
            dest_.append(fill, ' ');
        } else {
            dest_.insert(start_, fill, ' ');
        }
    }

private:
    std::string& dest_;
    std::size_t start_;
    padding_info info_;
};

// 单个 pattern flag 的格式化器。所有时间 flag 共享按秒缓存的 tm。
class flag_formatter {
public:
    explicit flag_formatter(padding_info info = {}) : info_(info) {}
    virtual ~flag_formatter() = default;

    auto format(const Message& message, const std::tm& time, std::string& dest) const -> void {
        scoped_padder pad(dest, info_);
        format_value(message, time, dest);
    }

private:
    virtual auto format_value(const Message&, const std::tm&, std::string&) const -> void = 0;
    padding_info info_;
};

// 固定字面量（含 %%），无 padding。
class literal_formatter final : public flag_formatter {
public:
    explicit literal_formatter(std::string text) : text_(std::move(text)) {}

private:
    auto format_value(const Message&, const std::tm&, std::string& dest) const -> void override {
        dest.append(text_);
    }
    std::string text_;
};

// 用户消息 payload（%v）。
class payload_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        dest.append(msg.formatted());
    }
};

class name_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        dest.append(msg.logger_name());
    }
};

class level_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        dest.append(level_name(msg.level()));
    }
};

class short_level_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        auto name = level_name(msg.level());
        if (!name.empty()) {
            dest.push_back(static_cast<char>(name.front()));
        }
    }
};

// 源位置 file:line（%@）。
class source_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        auto location = msg.location();
        auto filename = std::filesystem::path(location.file).filename().string();
        dest.append(std::format("{}:{}", filename, location.line));
    }
};

// 整数时间字段的通用 formatter（年/月/日/时/分/秒）。
class int_time_formatter final : public flag_formatter {
public:
    using field_getter = int (*)(const std::tm&);
    int_time_formatter(field_getter getter, int width, padding_info info)
        : flag_formatter(info), getter_(getter), width_(width) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        dest.append(std::format("{:0>{}}", getter_(time), width_));
    }
    field_getter getter_;
    int width_;
};

// 亚秒字段（毫秒 %e / 微秒 %f）。
class fraction_formatter final : public flag_formatter {
public:
    fraction_formatter(std::chrono::milliseconds::rep divisor, int width, padding_info info)
        : flag_formatter(info), divisor_(divisor), width_(width) {}

private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        auto count = std::chrono::duration_cast<std::chrono::microseconds>(
                         msg.timestamp().time_since_epoch())
                         .count();
        auto value = count % divisor_;
        dest.append(std::format("{:0>{}}", value, width_));
    }
    std::chrono::microseconds::rep divisor_;
    int width_;
};

// 字段取值函数（供 int_time_formatter 使用）。
inline auto tm_year(const std::tm& t) -> int { return t.tm_year + 1900; }
inline auto tm_short_year(const std::tm& t) -> int { return t.tm_year % 100; }
inline auto tm_month(const std::tm& t) -> int { return t.tm_mon + 1; }
inline auto tm_day(const std::tm& t) -> int { return t.tm_mday; }
inline auto tm_hour(const std::tm& t) -> int { return t.tm_hour; }
inline auto tm_minute(const std::tm& t) -> int { return t.tm_min; }
inline auto tm_second(const std::tm& t) -> int { return t.tm_sec; }

// 解析 padding：支持 %-Nflag（左对齐）、%Nflag（右对齐）。
inline auto parse_padding(std::string_view pattern, std::size_t& index) -> padding_info {
    padding_info info;
    if (index < pattern.size() && pattern[index] == '-') {
        info.align = padding_info::side::left;
        ++index;
    }
    auto start = index;
    while (index < pattern.size() && pattern[index] >= '0' && pattern[index] <= '9') {
        ++index;
    }
    if (index > start) {
        info.width = static_cast<std::size_t>(std::stoi(std::string(pattern.substr(start, index - start))));
    }
    return info;
}

// 将单个 flag 编译为 formatter。need_localtime 标记是否用到时间字段。
inline auto make_flag(char flag, padding_info info, bool& need_localtime)
    -> std::unique_ptr<flag_formatter> {
    using enum padding_info::side;
    switch (flag) {
    case 'v': return std::make_unique<payload_formatter>(info);
    case 'n': return std::make_unique<name_formatter>(info);
    case 'l': return std::make_unique<level_formatter>(info);
    case 'L': return std::make_unique<short_level_formatter>(info);
    case '@': return std::make_unique<source_formatter>(info);
    case 'Y': need_localtime = true; return std::make_unique<int_time_formatter>(tm_year, 4, info);
    case 'y': need_localtime = true; return std::make_unique<int_time_formatter>(tm_short_year, 2, info);
    case 'm': need_localtime = true; return std::make_unique<int_time_formatter>(tm_month, 2, info);
    case 'd': need_localtime = true; return std::make_unique<int_time_formatter>(tm_day, 2, info);
    case 'H': need_localtime = true; return std::make_unique<int_time_formatter>(tm_hour, 2, info);
    case 'M': need_localtime = true; return std::make_unique<int_time_formatter>(tm_minute, 2, info);
    case 'S': need_localtime = true; return std::make_unique<int_time_formatter>(tm_second, 2, info);
    case 'e': return std::make_unique<fraction_formatter>(1000, 3, info);
    case 'f': return std::make_unique<fraction_formatter>(1000000, 6, info);
    case '%': return std::make_unique<literal_formatter>("%");
    default:  return std::make_unique<literal_formatter>(std::string("%") + flag);
    }
}

// 自定义 flag 的内部类型擦除：原型可复制，编译 pattern 时按出现次数复制处理器。
class custom_flag_holder {
public:
    virtual ~custom_flag_holder() = default;
    virtual auto clone() const -> std::unique_ptr<custom_flag_holder> = 0;
    virtual auto make_formatter(padding_info info) const -> std::unique_ptr<flag_formatter> = 0;
};

template <PatternFlag T>
class custom_flag_formatter final : public flag_formatter {
public:
    custom_flag_formatter(T handler, padding_info info)
        : flag_formatter(info), handler_(std::move(handler)) {}

private:
    auto format_value(const Message& message, const std::tm& time, std::string& dest) const
        -> void override {
        handler_.format(message, time, dest);
    }
    mutable T handler_;
};

template <PatternFlag T>
class custom_flag_holder_model final : public custom_flag_holder {
public:
    explicit custom_flag_holder_model(T handler) : handler_(std::move(handler)) {}

    auto clone() const -> std::unique_ptr<custom_flag_holder> override {
        return std::make_unique<custom_flag_holder_model>(handler_);
    }

    auto make_formatter(padding_info info) const -> std::unique_ptr<flag_formatter> override {
        return std::make_unique<custom_flag_formatter<T>>(handler_, info);
    }

private:
    T handler_;
};

} // namespace detail

// formatter 抽象基类：原型模式，每个 sink 持有独立 clone。
export class Formatter {
public:
    virtual ~Formatter() = default;
    virtual auto format(const Message& message, std::string& dest) -> void = 0;
    virtual auto set_pattern(std::string_view) -> void {
        ylog_detail_errors::fail_config(
            "this Formatter does not support set_pattern; use PatternFormatter or inject a replacement");
    }
    virtual auto clone() const -> std::unique_ptr<Formatter> = 0;
    virtual auto pattern() const -> std::string_view = 0;
};

// 可编译的 pattern formatter。
// pattern 在 set_pattern 时编译为 flag formatter 列表；时间字段按秒缓存。
export class PatternFormatter final : public Formatter {
public:
    explicit PatternFormatter(std::string_view pattern = default_pattern) {
        set_pattern(pattern);
    }

    auto clone() const -> std::unique_ptr<Formatter> override {
        auto result = std::make_unique<PatternFormatter>(pattern_);
        for (const auto& [flag, holder] : custom_flags_) {
            result->custom_flags_.emplace(flag, holder->clone());
        }
        result->recompile_();
        return result;
    }

    auto pattern() const -> std::string_view override { return pattern_; }

    template <PatternFlag T>
    auto add_flag(char flag, T handler) -> void {
        if (custom_flags_.contains(flag)) {
            ylog_detail_errors::fail_config(std::format("duplicate custom pattern flag '{}'", flag));
        }
        custom_flags_.emplace(
            flag, std::make_unique<detail::custom_flag_holder_model<T>>(std::move(handler)));
        recompile_();
    }

    auto set_pattern(std::string_view pattern) -> void override {
        pattern_ = pattern;
        recompile_();
    }

    auto format(const Message& message, std::string& dest) -> void override {
        if (need_localtime_) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                message.timestamp().time_since_epoch());
            if (secs != last_log_secs_) {
                cached_tm_ = detail::localtime_threadsafe(
                    std::chrono::system_clock::to_time_t(message.timestamp()));
                last_log_secs_ = secs;
            }
        }
        for (const auto& formatter : formatters_) {
            formatter->format(message, cached_tm_, dest);
        }
        dest.append(eol_);
    }

private:
    auto recompile_() -> void {
        formatters_.clear();
        need_localtime_ = !custom_flags_.empty();
        compile_pattern_(pattern_);
        last_log_secs_ = std::chrono::seconds{-1};
    }

    auto compile_pattern_(std::string_view pattern) -> void {
        std::string literal;
        auto flush_literal = [&] {
            if (!literal.empty()) {
                formatters_.push_back(std::make_unique<detail::literal_formatter>(literal));
                literal.clear();
            }
        };

        std::size_t index = 0;
        while (index < pattern.size()) {
            if (pattern[index] != '%') {
                literal.push_back(pattern[index++]);
                continue;
            }
            ++index;  // 跳过 '%'
            if (index >= pattern.size()) {
                literal.push_back('%');
                break;
            }

            auto info = detail::parse_padding(pattern, index);
            if (index >= pattern.size()) {
                break;
            }
            auto flag = pattern[index++];
            flush_literal();
            if (auto custom = custom_flags_.find(flag); custom != custom_flags_.end()) {
                formatters_.push_back(custom->second->make_formatter(info));
            } else {
                formatters_.push_back(detail::make_flag(flag, info, need_localtime_));
            }
        }
        flush_literal();
    }

    std::string pattern_{default_pattern};
    std::string eol_{"\n"};
    std::vector<std::unique_ptr<detail::flag_formatter>> formatters_;
    std::unordered_map<char, std::unique_ptr<detail::custom_flag_holder>> custom_flags_;
    bool need_localtime_{false};

    // 按秒缓存的 localtime 结果，避免每条日志都调用 localtime。
    std::chrono::seconds last_log_secs_{-1};
    std::tm cached_tm_{};
};

} // namespace ylog
