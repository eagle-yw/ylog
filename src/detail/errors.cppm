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

// 各 sink 配置结构体的默认 err_handler：故障时写 stderr，同一秒内只报一次。
// 默认值集中在这里，避免每个 sink 各带一份限频状态（ADR-0009）。
export auto make_default_err_handler() -> std::function<void(std::string_view)> {
    struct state {
        std::mutex mutex;
        std::chrono::steady_clock::time_point last_report{};
    };

    auto shared = std::make_shared<state>();
    return [shared](std::string_view message) {
        auto lock = std::lock_guard(shared->mutex);
        auto now = std::chrono::steady_clock::now();
        if (now - shared->last_report < std::chrono::seconds{1}) {
            return;
        }
        shared->last_report = now;
        std::println(std::cerr, "[ylog] {}", message);
    };
}

// 热路径失败的上报锁存：连续失败只上报一次，成功之后由调用方复位。
// 与默认 handler 的限频是两件事：前者压住同一次故障的重复上报，后者压住频率。
export class failure_latch {
public:
    // 进入失败状态时返回 true，此后持续失败返回 false。
    auto enter_failure() -> bool {
        if (reported_) {
            return false;
        }
        reported_ = true;
        return true;
    }

    auto reset() -> void { reported_ = false; }

private:
    bool reported_{false};
};

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

// 把一次可能抛出的调用收敛成一次上报：异常文本交给 on_failure，调用方继续工作。
// 必须是模板：-fno-exceptions 下 try/catch 的语义检查发生在实例化期，
// 被 if constexpr 丢弃的分支才不会报「exception handling disabled」。
export template <typename Action, typename OnFailure>
auto guard_call(Action&& action, OnFailure&& on_failure) -> void {
    if constexpr (exceptions_enabled) {
        try {
            std::invoke(action);
        } catch (const std::exception& failure) {
            // 上报本身也在热路径上：handler 再抛一次不能把异常带出去。
            safe_err_handler(on_failure, std::string_view(failure.what()));
        } catch (...) {
            safe_err_handler(on_failure, std::string_view("未知异常"));
        }
    } else {
        std::invoke(action);
    }
}

}  // namespace ylog_detail_errors
