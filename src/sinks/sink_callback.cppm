export module ylog:sink.callback;

import std;
import :sink;
import ylog_detail_errors;

namespace ylog {

// 回调收到的是格式化后的整行（借用视图，只在调用期间有效）；级别与 logger 名需由
// pattern 自带，见 ADR-0009 的 Known Divergence。
export struct CallbackConfig {
    std::function<void(std::string_view)> fn;
    std::function<void(std::string_view)> err_handler{
        ylog_detail_errors::make_default_err_handler()};
};

// 把行交给调用方提供的函数。实现 Sink 概念。
//
// 回调在 AnySink::output_mutex_ 下调用：回调内不得再向同一个 logger 记录（重入不受支持，
// 与 pattern flag、err_handler 的既有约定一致）。回调抛出的异常收敛成一次 err_handler
// 上报，logger 的其它 sink 与后续日志不受影响。
export class CallbackSink {
public:
    explicit CallbackSink(CallbackConfig config)
        : config_(std::move(config)) {
        if (!config_.fn) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                "CallbackSink: fn must not be empty");
        }
    }

    auto write(std::string_view line) -> void {
        ylog_detail_errors::guard_call([this, line] { config_.fn(line); }, config_.err_handler);
    }

private:
    CallbackConfig config_;
};

}  // namespace ylog
