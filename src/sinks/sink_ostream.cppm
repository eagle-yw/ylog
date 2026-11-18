export module ylog:sink.ostream;

import std;
import :sink;
import ylog_detail_errors;
import ylog_detail_os;

namespace ylog {

namespace detail {

// Automatic 在配置期定型成 Never/Always：热路径只读结果，不再探测终端。
inline auto resolve_color_mode(ColorMode mode, bool attached_to_terminal) -> ColorMode {
    if (mode != ColorMode::Automatic) {
        return mode;
    }
    return attached_to_terminal ? ColorMode::Always : ColorMode::Never;
}

// 只有 std::cout / std::cerr 能可靠判定终端；其余 stream（含 ostringstream）一律按非终端处理。
inline auto stream_is_terminal(std::ostream& stream) -> bool {
    if (&stream == &std::cout) {
        return ylog_detail_os::stdout_is_tty();
    }
    if (&stream == &std::cerr) {
        return ylog_detail_os::stderr_is_tty();
    }
    return false;
}

inline auto fail_ostream_config(std::string_view reason) -> void {
    ylog_detail_errors::fail_config(std::format("OstreamSink: {}", reason));
}

}  // namespace detail

// 任意 ostream 的配置。sink 不拥有 stream，生命周期由调用方保证。
export struct OstreamConfig {
    std::ostream* stream{nullptr};
    ColorMode color{ColorMode::Never};
    std::function<void(std::string_view)> err_handler{
        ylog_detail_errors::make_default_err_handler()};
};

// 输出到调用方给定的 ostream。实现 Sink 概念；stderr 变体由 OstreamSink{&std::cerr} 表达。
export class OstreamSink {
public:
    explicit OstreamSink(OstreamConfig config)
        : config_(std::move(config)) {
        if (config_.stream == nullptr) {
            detail::fail_ostream_config("stream must not be null");
        }
        color_ =
            detail::resolve_color_mode(config_.color, detail::stream_is_terminal(*config_.stream));
    }

    auto write(std::string_view line) -> void {
        config_.stream->write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!config_.stream->bad()) {
            return;
        }
        report_("write");
    }

    auto flush() -> void {
        config_.stream->flush();
        if (!config_.stream->bad()) {
            reported_ = false;
            return;
        }
        report_("flush");
    }

    auto color_mode() const -> ColorMode { return color_; }

private:
    // 连续失败只上报一次：下一次 flush 成功才复位。上报后清掉 badbit，
    // 让后续写入仍有机会恢复，而不是让 sink 永久死掉。
    auto report_(std::string_view operation) -> void {
        config_.stream->clear();
        if (reported_) {
            return;
        }
        reported_ = true;
        ylog_detail_errors::safe_err_handler(config_.err_handler,
                                             std::format("ostream sink {} failed", operation));
    }

    OstreamConfig config_;
    ColorMode color_{ColorMode::Never};
    bool reported_{false};
};

}  // namespace ylog
