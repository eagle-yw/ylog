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

// 默认上报：写 stderr，约每秒一条，其余计数后静默丢弃（下次放行时附带抑制条数）。
// 状态放在 shared_ptr 里：ErrorHandler 要求可拷贝，拷贝之间共享同一份限频。
class rate_limited_reporter {
public:
    auto operator()(std::string_view text) const -> void {
        constexpr auto interval_ns = std::chrono::seconds(1).count() * 1'000'000'000LL;
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

        auto last = state_->last_report_ns.load(std::memory_order_relaxed);
        if (last != 0 && now_ns - last < interval_ns) {
            state_->suppressed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!state_->last_report_ns.compare_exchange_strong(last, now_ns,
                                                            std::memory_order_relaxed)) {
            state_->suppressed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto suppressed = state_->suppressed.exchange(0, std::memory_order_relaxed);
        if (suppressed > 0) {
            std::println(std::cerr, "ylog: {}（此前已抑制 {} 条同类上报）", text, suppressed);
        } else {
            std::println(std::cerr, "ylog: {}", text);
        }
    }

private:
    struct state {
        std::atomic<long long> last_report_ns{0};
        std::atomic<long long> suppressed{0};
    };

    std::shared_ptr<state> state_ = std::make_shared<state>();
};

// 默认 err_handler 的唯一实现：sink 配置结构体与 `AsyncLoggerConfig` 都从这里取值。
// 每次调用各持一份窗口——限频是 per-sink 的，不是全局的。
export auto make_default_err_handler() -> std::function<void(std::string_view)> {
    return rate_limited_reporter{};
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
