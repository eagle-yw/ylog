export module ylog:level;

import std;

namespace ylog {

// 日志级别。数值越小越严重，与过滤方向一致。
export enum class Level : int {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off
};

export constexpr std::size_t level_count = 7;

// 显式名称表：反射只属于 ylog:message 的 detail，不进入其他模块。
namespace detail {

inline constexpr std::array<std::string_view, level_count> level_names{
    "Trace", "Debug", "Info", "Warning", "Error", "Critical", "Off"};

} // namespace detail

export constexpr auto level_name(Level value) -> std::string_view {
    auto index = static_cast<std::size_t>(value);
    return index < level_count ? detail::level_names[index] : "unknown";
}

export constexpr auto level_from_string(std::string_view text) -> Level {
    for (std::size_t i = 0; i < level_count; ++i) {
        if (text == detail::level_names[i]) {
            return static_cast<Level>(i);
        }
    }
    return Level::Info;
}

} // namespace ylog
