export module ylog_detail_errors;

import std;

namespace ylog_detail_errors {

export inline constexpr bool exceptions_enabled =
#ifdef YLOG_NO_EXCEPTIONS
    false;
#else
    true;
#endif

export template <class Error = std::invalid_argument, bool Enabled = exceptions_enabled>
auto fail_config(std::string_view reason) -> void {
    if constexpr (Enabled) {
        throw Error(std::string(reason));
    } else {
        // 无异常构建下不静默丢弃：先输出可操作的错误信息，再终止进程。
        std::println(std::cerr, "ylog: invalid configuration: {}", reason);
        std::terminate();
    }
}

export template <bool Enabled = exceptions_enabled, class F>
auto safe_err_handler(F&& handler, std::string_view message) -> void {
    if constexpr (Enabled) {
        try {
            std::invoke(std::forward<F>(handler), message);
        } catch (...) {
        }
    } else {
        std::invoke(std::forward<F>(handler), message);
    }
}

} // namespace ylog_detail_errors
