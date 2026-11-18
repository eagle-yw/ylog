export module ylog:sink.file;

import std;
import :sink;
import ylog_detail_errors;

namespace ylog {

// 输出到文件。实现 Sink 概念。
export class FileSink {
public:
    explicit FileSink(std::string_view filename)
        : stream_(std::string(filename)) {
        if (!stream_.is_open()) {
            ylog_detail_errors::fail_config<std::ios_base::failure>(
                std::format("Failed to open log file: {}", filename));
        }
    }

    FileSink(const FileSink&) = delete;
    auto operator=(const FileSink&) -> FileSink& = delete;
    FileSink(FileSink&&) noexcept = default;
    auto operator=(FileSink&&) noexcept -> FileSink& = default;
    ~FileSink() = default;

    auto write(std::string_view line) -> void {
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
    }

    auto flush() -> void { stream_.flush(); }

private:
    std::ofstream stream_;
};

} // namespace ylog
