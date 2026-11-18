module;

// assert 是宏，`import std;` 带不进来。
#include <cassert>

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
        // 断言表达「调用方违反了前置条件」，但它在 NDEBUG 下会被编译掉，所以终止必须
        // 无条件跟在后面：断言给调试器一个断点，终止负责发布构建里的响亮失败。
        // 两者不能互相替代，也不能只留其中一个。
        assert(false && "ylog: invalid configuration");
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

}  // namespace ylog_detail_errors
