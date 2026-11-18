export module ylog:formatter;

import std;
import :level;
import :message;
import ylog_detail_errors;
import ylog_detail_os;

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

template <typename T>
inline auto append_int(std::string& dest, T value) -> void {
    dest.append(std::to_string(value));
}

// 定宽补零，用于 %m/%d/%H 等两位字段。
inline auto append_zero_padded(std::string& dest, std::int64_t value, int width) -> void {
    dest.append(std::format("{:0>{}}", value, width));
}

// 秒以下部分（纳秒），%e/%f/%F 与 %+ 共用；与 spdlog 的 time_fraction 同义。
inline auto nanosecond_fraction(const Message& message) -> std::int64_t {
    auto duration = message.timestamp().time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    return (nanos - std::chrono::duration_cast<std::chrono::nanoseconds>(seconds)).count();
}

// 路径最后一段；%@/%s 与 %+ 使用。
inline auto basename(std::string_view path) -> std::string_view {
    auto position = path.find_last_of("/\\");
    return position == std::string_view::npos ? path : path.substr(position + 1);
}

// 与 spdlog 的 source_loc::empty() 同义。
inline auto source_empty(const Message& message) -> bool {
    return message.location().empty();
}

// 对齐/宽度/截断信息，解析 pattern 时填充。
struct padding_info {
    // 空白补在值的哪一侧：left = 值左侧（默认，值右对齐）、right = 值右侧（前缀 '-'）、
    // center = 两侧（前缀 '='）。
    enum class side { left, right, center };

    std::size_t width{0};
    side align{side::left};
    bool truncate{false};
    bool enabled{false};
};

// 最近一次格式化中 `%^`/`%$` 标出的区间（[start, end)）。
struct color_range {
    std::size_t start{0};
    std::size_t end{0};
};

// RAII：析构时补齐空格或截断，规则与 spdlog 的 scoped_padder 一致。
class scoped_padder {
public:
    // 表示“按实际写入长度计算”；固定字段宽度（如 %c 的 24）由 flag 传入。
    static inline constexpr std::size_t measure = static_cast<std::size_t>(-1);

    scoped_padder(std::string& dest, const padding_info& info, std::size_t fixed_size = measure)
        : dest_(dest), start_(dest.size()), info_(info), fixed_size_(fixed_size) {}

    scoped_padder(const scoped_padder&) = delete;
    auto operator=(const scoped_padder&) -> scoped_padder& = delete;

    ~scoped_padder() {
        if (!info_.enabled) {
            return;
        }
        auto written = dest_.size() - start_;
        auto content = fixed_size_ == measure ? written : fixed_size_;
        auto fill = static_cast<std::ptrdiff_t>(info_.width) -
                    static_cast<std::ptrdiff_t>(content);
        if (fill > 0) {
            auto count = static_cast<std::size_t>(fill);
            switch (info_.align) {
            case padding_info::side::left: dest_.insert(start_, count, ' '); break;
            case padding_info::side::right: dest_.append(count, ' '); break;
            case padding_info::side::center: {
                auto head = count / 2;
                dest_.insert(start_, head, ' ');
                dest_.append(count - head, ' ');
                break;
            }
            }
        } else if (fill < 0 && info_.truncate) {
            // 与 spdlog 一致：从尾部截掉超出部分，保留最左侧 width 个字符。
            auto target = static_cast<std::ptrdiff_t>(dest_.size()) + fill;
            if (target < static_cast<std::ptrdiff_t>(start_)) {
                target = static_cast<std::ptrdiff_t>(start_);
            }
            dest_.resize(static_cast<std::size_t>(target));
        }
    }

private:
    std::string& dest_;
    std::size_t start_;
    padding_info info_;
    std::size_t fixed_size_;
};

// 单个 pattern flag 的格式化器。所有时间 flag 共享按秒缓存的 tm。
class flag_formatter {
public:
    explicit flag_formatter(padding_info info = {},
                            std::size_t fixed_size = scoped_padder::measure)
        : info_(info), fixed_size_(fixed_size) {}
    virtual ~flag_formatter() = default;

    auto format(const Message& message, const std::tm& time, std::string& dest) const -> void {
        scoped_padder pad(dest, info_, fixed_size_);
        format_value(message, time, dest);
    }

private:
    virtual auto format_value(const Message&, const std::tm&, std::string&) const -> void = 0;

    padding_info info_;
    std::size_t fixed_size_;
};

// 固定字面量（含 %% 与未知 flag）。与 spdlog 一致，不受 padding 影响。
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

// logger 名（%n）。
class name_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        dest.append(msg.logger_name());
    }
};

// 完整 level 名（%l）。名称表来自 ylog:level，与 spdlog 的小写名有意不同。
class level_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        dest.append(level_name(msg.level()));
    }
};

// 单字符 level（%L）：与 spdlog 的短名一致（Info -> I）。
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

// 线程 id（%t）：构造 Message 时捕获，异步 sink 因此报告生产者线程。
class thread_id_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        append_int(dest, msg.thread_id());
    }
};

// 进程 id（%P）。
class pid_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message&, const std::tm&, std::string& dest) const -> void override {
        append_int(dest, ylog_detail_os::process_id());
    }
};

// 星期/月份英文名，与 spdlog 的表一致。
inline constexpr std::array<std::string_view, 7> short_weekdays{
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
inline constexpr std::array<std::string_view, 7> long_weekdays{
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
inline constexpr std::array<std::string_view, 12> short_months{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
inline constexpr std::array<std::string_view, 12> long_months{
    "January", "February", "March", "April", "May", "June", "July", "August",
    "September", "October", "November", "December"};

inline auto short_weekday(const std::tm& time) -> std::string_view {
    return short_weekdays[static_cast<std::size_t>(time.tm_wday)];
}
inline auto long_weekday(const std::tm& time) -> std::string_view {
    return long_weekdays[static_cast<std::size_t>(time.tm_wday)];
}
inline auto short_month(const std::tm& time) -> std::string_view {
    return short_months[static_cast<std::size_t>(time.tm_mon)];
}
inline auto long_month(const std::tm& time) -> std::string_view {
    return long_months[static_cast<std::size_t>(time.tm_mon)];
}

// 名字类时间字段（%a/%A/%b/%h/%B）。
class name_list_formatter final : public flag_formatter {
public:
    using getter = std::string_view (*)(const std::tm&);

    name_list_formatter(getter get_value, padding_info info)
        : flag_formatter(info), get_value_(get_value) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        dest.append(get_value_(time));
    }
    getter get_value_;
};

// 整数时间字段（%C/%y/%m/%d/%H/%M/%S）。zero_width 为 0 时不补零（%Y）。
class int_time_formatter final : public flag_formatter {
public:
    using field_getter = int (*)(const std::tm&);

    int_time_formatter(field_getter getter, int zero_width, padding_info info)
        : flag_formatter(info), getter_(getter), zero_width_(zero_width) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        if (zero_width_ == 0) {
            append_int(dest, getter_(time));
            return;
        }
        append_zero_padded(dest, getter_(time), zero_width_);
    }
    field_getter getter_;
    int zero_width_;
};

// 12 小时制换算：0 点输出 00、12 点输出 12（与 spdlog 的 to12h 一致）。
inline auto to12h(const std::tm& time) -> int {
    return time.tm_hour > 12 ? time.tm_hour - 12 : time.tm_hour;
}
inline auto ampm(const std::tm& time) -> std::string_view {
    return time.tm_hour >= 12 ? "PM" : "AM";
}

// `%c`：Thu Nov 18 10:20:30 2026，日不补零。
class c_formatter final : public flag_formatter {
public:
    explicit c_formatter(padding_info info) : flag_formatter(info, 24) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        dest.append(short_weekday(time));
        dest.push_back(' ');
        dest.append(short_month(time));
        dest.push_back(' ');
        append_int(dest, time.tm_mday);
        dest.append(std::format(" {:02}:{:02}:{:02} ", time.tm_hour, time.tm_min, time.tm_sec));
        append_int(dest, time.tm_year + 1900);
    }
};

// `%D`/`%x`：MM/DD/YY。
class date_formatter final : public flag_formatter {
public:
    explicit date_formatter(padding_info info) : flag_formatter(info, 8) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        dest.append(std::format(
            "{:02}/{:02}/{:02}", time.tm_mon + 1, time.tm_mday, time.tm_year % 100));
    }
};

// `%p`：AM/PM。
class ampm_formatter final : public flag_formatter {
public:
    explicit ampm_formatter(padding_info info) : flag_formatter(info, 2) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        dest.append(ampm(time));
    }
};

// `%r`：hh:mm:ss AM/PM。
class clock12_formatter final : public flag_formatter {
public:
    explicit clock12_formatter(padding_info info) : flag_formatter(info, 11) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        dest.append(std::format(
            "{:02}:{:02}:{:02} {}", to12h(time), time.tm_min, time.tm_sec, ampm(time)));
    }
};

// `%R`：HH:MM。
class clock_hm_formatter final : public flag_formatter {
public:
    explicit clock_hm_formatter(padding_info info) : flag_formatter(info, 5) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        dest.append(std::format("{:02}:{:02}", time.tm_hour, time.tm_min));
    }
};

// `%T`/`%X`：HH:MM:SS。
class clock_hms_formatter final : public flag_formatter {
public:
    explicit clock_hms_formatter(padding_info info) : flag_formatter(info, 8) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        dest.append(std::format("{:02}:{:02}:{:02}", time.tm_hour, time.tm_min, time.tm_sec));
    }
};

// `%z`：±HH:MM 的 UTC 偏移。
class timezone_formatter final : public flag_formatter {
public:
    explicit timezone_formatter(padding_info info) : flag_formatter(info, 6) {}

private:
    auto format_value(const Message&, const std::tm& time, std::string& dest) const -> void override {
        auto total_minutes = ylog_detail_os::utc_offset_minutes(time);
        auto negative = total_minutes < 0;
        if (negative) {
            total_minutes = -total_minutes;
        }
        dest.push_back(negative ? '-' : '+');
        dest.append(std::format("{:02}:{:02}", total_minutes / 60, total_minutes % 60));
    }
};

// 秒以下字段（%e 毫秒 / %f 微秒 / %F 纳秒），整秒余数补 3/6/9 位。
class fraction_formatter final : public flag_formatter {
public:
    fraction_formatter(int width, padding_info info) : flag_formatter(info), width_(width) {
        divisor_ = 1;
        for (int index = width; index < 9; ++index) {
            divisor_ *= 10;
        }
    }

private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        append_zero_padded(dest, nanosecond_fraction(msg) / divisor_, width_);
    }
    int width_;
    std::int64_t divisor_{1};
};

// `%E`：epoch 起的整秒数。
class epoch_formatter final : public flag_formatter {
public:
    explicit epoch_formatter(padding_info info) : flag_formatter(info, 10) {}

private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            msg.timestamp().time_since_epoch());
        append_int(dest, seconds.count());
    }
};

// `%@`：完整文件名:行号。无来源时不输出。
class source_location_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        if (source_empty(msg)) {
            return;
        }
        dest.append(msg.location().file);
        dest.push_back(':');
        append_int(dest, msg.location().line);
    }
};

// `%g`：完整源文件名。
class source_filename_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        if (source_empty(msg)) {
            return;
        }
        dest.append(msg.location().file);
    }
};

// `%s`：去目录的短文件名。
class short_filename_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        if (source_empty(msg)) {
            return;
        }
        dest.append(basename(msg.location().file));
    }
};

// `%#`：源行号。
class source_line_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        if (source_empty(msg)) {
            return;
        }
        append_int(dest, msg.location().line);
    }
};

// `%!`：源函数名。
class source_function_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        if (source_empty(msg)) {
            return;
        }
        dest.append(msg.location().function);
    }
};

// `%^`/`%$`：只记录色彩区间，不写转义序列（与 spdlog 一致）。
class color_start_formatter final : public flag_formatter {
public:
    explicit color_start_formatter(color_range& color) : color_(&color) {}

private:
    auto format_value(const Message&, const std::tm&, std::string& dest) const -> void override {
        color_->start = dest.size();
    }
    color_range* color_;
};

class color_stop_formatter final : public flag_formatter {
public:
    explicit color_stop_formatter(color_range& color) : color_(&color) {}

private:
    auto format_value(const Message&, const std::tm&, std::string& dest) const -> void override {
        color_->end = dest.size();
    }
    color_range* color_;
};

// `%u`/`%i`/`%o`/`%O`：距上一条消息的纳秒/微秒/毫秒/秒数。
// 每个 flag 实例独立计时，初始值为构造时刻（与 spdlog 一致）。
template <typename Units>
class elapsed_formatter final : public flag_formatter {
public:
    explicit elapsed_formatter(padding_info info)
        : flag_formatter(std::move(info)), last_message_time_(std::chrono::system_clock::now()) {}

private:
    auto format_value(const Message& msg, const std::tm&, std::string& dest) const -> void override {
        auto delta = std::max(
            msg.timestamp() - last_message_time_, std::chrono::system_clock::duration::zero());
        last_message_time_ = msg.timestamp();
        dest.append(std::to_string(std::chrono::duration_cast<Units>(delta).count()));
    }
    mutable std::chrono::system_clock::time_point last_message_time_;
};

// `%&`：MDC。Ylog 尚无 MDC 容器，识别该 flag 但输出为空，
// 与 spdlog 在 MDC 为空时的行为一致。
class mdc_formatter final : public flag_formatter {
public:
    using flag_formatter::flag_formatter;
private:
    auto format_value(const Message&, const std::tm&, std::string&) const -> void override {}
};

// `%+`：默认完整格式，等价于
// `[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%s:#] %v`（无来源/无 logger 名时省略对应段）。
// 与 spdlog 一致，`%+` 忽略 padding。
class full_formatter final : public flag_formatter {
public:
    explicit full_formatter(color_range& color) : color_(&color) {}

private:
    auto format_value(const Message& msg, const std::tm& time, std::string& dest) const
        -> void override {
        dest.append(std::format(
            "[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] ",
            time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
            time.tm_hour, time.tm_min, time.tm_sec, nanosecond_fraction(msg) / 1'000'000));

        if (!msg.logger_name().empty()) {
            dest.push_back('[');
            dest.append(msg.logger_name());
            dest.append("] ");
        }

        dest.push_back('[');
        color_->start = dest.size();
        dest.append(level_name(msg.level()));
        color_->end = dest.size();
        dest.append("] ");

        if (!source_empty(msg)) {
            dest.push_back('[');
            dest.append(basename(msg.location().file));
            dest.push_back(':');
            append_int(dest, msg.location().line);
            dest.append("] ");
        }
        dest.append(msg.formatted());
    }
    color_range* color_;
};

// 字段取值函数（供 int_time_formatter 使用）。
inline auto tm_year(const std::tm& t) -> int { return t.tm_year + 1900; }
inline auto tm_short_year(const std::tm& t) -> int { return t.tm_year % 100; }
inline auto tm_month(const std::tm& t) -> int { return t.tm_mon + 1; }
inline auto tm_day(const std::tm& t) -> int { return t.tm_mday; }
inline auto tm_hour(const std::tm& t) -> int { return t.tm_hour; }
inline auto tm_minute(const std::tm& t) -> int { return t.tm_min; }
inline auto tm_second(const std::tm& t) -> int { return t.tm_sec; }

// 解析 padding：可选 `-`/`=` 前缀、宽度（上限 64）与 `!` 截断标记。
// 与 spdlog 一致：前缀被消费但后面不是数字时，按无 padding 处理。
inline auto parse_padding(std::string_view pattern, std::size_t& index) -> padding_info {
    if (index >= pattern.size()) {
        return padding_info{};
    }
    auto align = padding_info::side::left;
    if (pattern[index] == '-') {
        align = padding_info::side::right;
        ++index;
    } else if (pattern[index] == '=') {
        align = padding_info::side::center;
        ++index;
    }

    auto is_digit = [](char value) { return value >= '0' && value <= '9'; };
    if (index >= pattern.size() || !is_digit(pattern[index])) {
        return padding_info{};
    }

    auto width = std::size_t{0};
    while (index < pattern.size() && is_digit(pattern[index])) {
        auto digit = static_cast<std::size_t>(pattern[index] - '0');
        width = std::min<std::size_t>(width * 10 + digit, 64);
        ++index;
    }

    auto truncate = false;
    if (index < pattern.size() && pattern[index] == '!') {
        truncate = true;
        ++index;
    }
    return padding_info{width, align, truncate, true};
}

// 将单个 flag 编译为 formatter。need_localtime 标记是否用到时间字段。
inline auto make_flag(char flag, padding_info info, bool& need_localtime, color_range& color)
    -> std::unique_ptr<flag_formatter> {
    switch (flag) {
    case 'v': return std::make_unique<payload_formatter>(info);
    case 'n': return std::make_unique<name_formatter>(info);
    case 'l': return std::make_unique<level_formatter>(info);
    case 'L': return std::make_unique<short_level_formatter>(info);
    case 't': return std::make_unique<thread_id_formatter>(info);
    case 'P': return std::make_unique<pid_formatter>(info);

    case '+':
        // spdlog 的 full_formatter 不使用 padding。
        need_localtime = true;
        return std::make_unique<full_formatter>(color);

    case 'a': need_localtime = true; return std::make_unique<name_list_formatter>(short_weekday, info);
    case 'A': need_localtime = true; return std::make_unique<name_list_formatter>(long_weekday, info);
    case 'b':
    case 'h': need_localtime = true; return std::make_unique<name_list_formatter>(short_month, info);
    case 'B': need_localtime = true; return std::make_unique<name_list_formatter>(long_month, info);
    case 'c': need_localtime = true; return std::make_unique<c_formatter>(info);
    case 'C':
        need_localtime = true;
        return std::make_unique<int_time_formatter>(tm_short_year, 2, info);
    case 'y':
        need_localtime = true;
        return std::make_unique<int_time_formatter>(tm_short_year, 2, info);
    case 'Y': need_localtime = true; return std::make_unique<int_time_formatter>(tm_year, 0, info);
    case 'D':
    case 'x': need_localtime = true; return std::make_unique<date_formatter>(info);
    case 'm': need_localtime = true; return std::make_unique<int_time_formatter>(tm_month, 2, info);
    case 'd': need_localtime = true; return std::make_unique<int_time_formatter>(tm_day, 2, info);
    case 'H': need_localtime = true; return std::make_unique<int_time_formatter>(tm_hour, 2, info);
    case 'I':
        need_localtime = true;
        return std::make_unique<int_time_formatter>(
            [](const std::tm& time) { return to12h(time); }, 2, info);
    case 'M': need_localtime = true; return std::make_unique<int_time_formatter>(tm_minute, 2, info);
    case 'S': need_localtime = true; return std::make_unique<int_time_formatter>(tm_second, 2, info);
    case 'p': need_localtime = true; return std::make_unique<ampm_formatter>(info);
    case 'r': need_localtime = true; return std::make_unique<clock12_formatter>(info);
    case 'R': need_localtime = true; return std::make_unique<clock_hm_formatter>(info);
    case 'T':
    case 'X': need_localtime = true; return std::make_unique<clock_hms_formatter>(info);
    case 'z': need_localtime = true; return std::make_unique<timezone_formatter>(info);

    case 'e': return std::make_unique<fraction_formatter>(3, info);
    case 'f': return std::make_unique<fraction_formatter>(6, info);
    case 'F': return std::make_unique<fraction_formatter>(9, info);
    case 'E': return std::make_unique<epoch_formatter>(info);

    case '@': return std::make_unique<source_location_formatter>(info);
    case 'g': return std::make_unique<source_filename_formatter>(info);
    case 's': return std::make_unique<short_filename_formatter>(info);
    case '#': return std::make_unique<source_line_formatter>(info);
    case '!': return std::make_unique<source_function_formatter>(info);

    case '^': return std::make_unique<color_start_formatter>(color);
    case '$': return std::make_unique<color_stop_formatter>(color);

    case 'u': return std::make_unique<elapsed_formatter<std::chrono::nanoseconds>>(info);
    case 'i': return std::make_unique<elapsed_formatter<std::chrono::microseconds>>(info);
    case 'o': return std::make_unique<elapsed_formatter<std::chrono::milliseconds>>(info);
    case 'O': return std::make_unique<elapsed_formatter<std::chrono::seconds>>(info);

    case '&': return std::make_unique<mdc_formatter>(info);

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

    // 最近一次 format 中 `%^`/`%$` 标出的区间；未出现时为 {0, 0}。
    auto color_range() const -> std::pair<std::size_t, std::size_t> {
        return {color_range_.start, color_range_.end};
    }

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
        color_range_ = {};
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
        color_range_ = {};
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
            flush_literal();
            ++index;  // 跳过 '%'
            auto info = detail::parse_padding(pattern, index);
            if (index >= pattern.size()) {
                break;  // 末尾单独的 '%' 被丢弃，与 spdlog 一致
            }
            auto flag = pattern[index++];
            if (auto custom = custom_flags_.find(flag); custom != custom_flags_.end()) {
                formatters_.push_back(custom->second->make_formatter(info));
            } else {
                formatters_.push_back(
                    detail::make_flag(flag, info, need_localtime_, color_range_));
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

    // `%^`/`%$` 记录的区间，随每次 format 重置。
    detail::color_range color_range_{};
};

} // namespace ylog
