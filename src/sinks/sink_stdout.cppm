export module ylog:sink.stdout;

import std;
import :sink;
import :sink.ostream;
import ylog_detail_errors;

namespace ylog {

// stdout 的配置。err_handler 只在热路径失败时被调用，热路径不抛。
export struct StdoutConfig {
    ColorMode color{ColorMode::Automatic};
    ErrorHandler err_handler{ylog_detail_errors::make_default_err_handler()};
};

// 输出到 stdout。实现 Sink 概念。
// 写与失败上报复用 OstreamSink：Automatic 因此按 isatty(stdout) 在配置期定型。
export class StdoutSink {
public:
    StdoutSink()
        : StdoutSink(StdoutConfig{}) {}

    explicit StdoutSink(StdoutConfig config)
        : sink_(OstreamConfig{.stream = &std::cout,
                              .color = config.color,
                              .err_handler = std::move(config.err_handler)}) {}

    auto write(std::string_view line) -> void { sink_.write(line); }

    auto flush() -> void { sink_.flush(); }

    auto color_mode() const -> ColorMode { return sink_.color_mode(); }

private:
    OstreamSink sink_;
};

}  // namespace ylog
