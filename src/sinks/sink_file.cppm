export module ylog:sink.file;

import std;
import :sink;
import ylog_detail_errors;

namespace ylog {

namespace detail {

inline auto fail_file_config(std::string_view reason) -> void {
    ylog_detail_errors::fail_config(std::format("FileSink: {}", reason));
}

inline auto open_file_stream(const std::string& filename, bool truncate) -> std::ofstream {
    auto mode = std::ios::out | std::ios::binary | (truncate ? std::ios::trunc : std::ios::app);
    auto stream = std::ofstream(filename, mode);
    if (!stream.is_open()) {
        fail_file_config(std::format("failed to open '{}'", filename));
    }
    return stream;
}

}  // namespace detail

// 单个文件的配置。默认追加写，truncate_on_open 打开时清空已有内容。
export struct FileConfig {
    std::string filename;
    bool truncate_on_open{false};
    std::function<void(std::string_view)> err_handler{
        ylog_detail_errors::make_default_err_handler()};
};

namespace detail {

inline auto validate_file_config(FileConfig config) -> FileConfig {
    if (config.filename.empty()) {
        fail_file_config("filename must not be empty");
    }
    return config;
}

}  // namespace detail

// 输出到文件。实现 Sink 概念。
export class FileSink {
public:
    // 便捷构造：等价于只给文件名的 FileConfig（追加写）。
    explicit FileSink(std::string_view filename)
        : FileSink(FileConfig{.filename = std::string(filename)}) {}

    explicit FileSink(FileConfig config)
        : config_(detail::validate_file_config(std::move(config))),
          stream_(detail::open_file_stream(config_.filename, config_.truncate_on_open)) {}

    FileSink(const FileSink&) = delete;
    auto operator=(const FileSink&) -> FileSink& = delete;
    FileSink(FileSink&&) noexcept = default;
    auto operator=(FileSink&&) noexcept -> FileSink& = default;
    ~FileSink() = default;

    auto write(std::string_view line) -> void {
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (stream_) {
            return;
        }
        report_("write");
    }

    auto flush() -> void {
        stream_.flush();
        if (stream_) {
            latch_.reset();
            return;
        }
        report_("flush");
    }

private:
    // 连续失败只上报一次：下一次 flush 成功才复位。上报后清掉流状态，
    // 让后续写入仍有机会恢复，而不是让 sink 永久死掉。
    auto report_(std::string_view operation) -> void {
        stream_.clear();
        if (!latch_.enter_failure()) {
            return;
        }
        ylog_detail_errors::safe_err_handler(
            config_.err_handler,
            std::format("file sink {} failed for '{}'", operation, config_.filename));
    }

    FileConfig config_;
    std::ofstream stream_;
    ylog_detail_errors::failure_latch latch_;
};

}  // namespace ylog
