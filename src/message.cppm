export module ylog:message;

import std;
import :level;
import ylog_detail_os;

namespace ylog {

// 源位置。默认构造即在调用点捕获位置，无需宏。
// 用法：logger.info({}, "msg {}", value) —— {} 触发默认构造捕获。
export struct SourceLocation {
    std::string_view file;
    std::uint32_t line;
    std::string_view function;

    SourceLocation(std::source_location loc = std::source_location::current())
        : file(loc.file_name()),
          line(loc.line()),
          function(loc.function_name()) {}

    // 显式指定位置：用于构造“无来源”的位置（line 为 0）或测试。
    SourceLocation(std::string_view file, std::uint32_t line, std::string_view function)
        : file(file),
          line(line),
          function(function) {}

    // 与 spdlog 的 source_loc::empty() 同义：无行号即视为无来源。
    auto empty() const -> bool { return line == 0 || file.empty(); }
};

// 编译期由反射生成的字段格式串元信息。
// 例如 struct Point { int x; double y; }; -> "x={} y={}"
namespace detail {

consteval auto make_field_format(std::meta::info type_info) -> std::meta::info {
    std::string result;
    auto members =
        std::meta::nonstatic_data_members_of(type_info, std::meta::access_context::unchecked());
    bool first = true;
    for (std::meta::info member : members) {
        if (!first) {
            result += " ";
        }
        result += std::meta::identifier_of(member);
        result += "={}";
        first = false;
    }
    return std::meta::reflect_constant_string(result);
}

template <typename T>
inline constexpr auto field_format_value = std::string_view([:make_field_format(^^T):]);

// 结构体字段数量（编译期）。
template <typename T>
consteval auto member_count() -> std::size_t {
    return std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()).size();
}

// 第 N 个字段的反射信息（供 splice 使用）。
template <typename T, std::size_t N>
consteval auto nth_member() -> std::meta::info {
    auto members =
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked());
    return members[N];
}

template <typename T>
auto to_tuple_impl(const T& object) {
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::tuple{object.[:nth_member<T, Is>():]...};
    }(std::make_index_sequence<member_count<T>()>{});
}

}  // namespace detail

// 供用户调用的编译期格式串生成器；反射信息留在 detail 内部。
export template <typename T>
consteval auto field_format() -> std::string_view {
    return detail::field_format_value<T>;
}

// 将聚合类型展开为字段 tuple。
export template <typename T>
auto to_tuple(const T& object) {
    return detail::to_tuple_impl(object);
}

// 反射包装器：让任意聚合类型可直接用于 std::format / std::println。
// 用法：std::println("{}", ylog::reflected(point));
export template <typename T>
struct Reflected {
    const T& value;
};

export template <typename T>
auto reflected(const T& object) -> Reflected<T> {
    return Reflected<T>{object};
}

// 日志消息：在构造时完成格式化，供 sink 消费。
// timestamp 与 thread_id 在构造点捕获：异步 sink 报告的是生产者线程与产生时刻。
export class Message {
public:
    Message(Level level,
            std::string_view logger_name,
            SourceLocation location,
            std::string_view formatted,
            std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now())
        : level_(level),
          logger_name_(logger_name),
          location_(location),
          formatted_(formatted),
          timestamp_(timestamp),
          thread_id_(ylog_detail_os::thread_id()) {}

    auto level() const -> Level { return level_; }
    auto logger_name() const -> std::string_view { return logger_name_; }
    auto location() const -> const SourceLocation& { return location_; }
    auto formatted() const -> std::string_view { return formatted_; }
    auto timestamp() const -> std::chrono::system_clock::time_point { return timestamp_; }
    auto thread_id() const -> std::size_t { return thread_id_; }

private:
    Level level_;
    std::string logger_name_;
    SourceLocation location_;
    std::string formatted_;
    std::chrono::system_clock::time_point timestamp_;
    std::size_t thread_id_;
};

}  // namespace ylog

// std::formatter 特化：让 Reflected<T> 可直接用于 std::format / println。
template <typename T>
struct std::formatter<ylog::Reflected<T>> {
    template <typename ParseContext>
    constexpr auto parse(ParseContext& context) {
        return context.begin();
    }

    template <typename FormatContext>
    auto format(const ylog::Reflected<T>& reflected, FormatContext& context) const {
        auto fields = ylog::to_tuple(reflected.value);
        return std::apply(
            [&context](auto&&... args) {
                return std::vformat_to(context.out(), ylog::detail::field_format_value<T>,
                                       std::make_format_args(args...));
            },
            fields);
    }
};
