export module ylog:sink.file.rotating;

import std;
import :sink;
import :sink.file.lifecycle;
import ylog_detail_errors;

namespace ylog {

namespace detail {

// 热路径上报文案里的 sink 名（如 "rotating file sink write failed for ..."）。
// 配置期前缀是另一套文本，由下面的 fail_rotating_config 给出类型名 "RotatingFileSink"。
inline constexpr std::string_view rotating_sink_name{"rotating file sink"};

inline auto fail_rotating_config(std::string_view reason) -> void {
    ylog_detail_errors::fail_config(std::format("RotatingFileSink: {}", reason));
}

inline auto backup_path(const std::string& filename, std::size_t index) -> std::filesystem::path {
    auto path = std::filesystem::path(filename);
    auto [stem, extension] = split_stem_and_extension(filename);
    return path.parent_path() / std::format("{}.{}{}", stem, index, extension);
}

}  // namespace detail

export struct RotatingFileConfig {
    std::string filename;
    std::size_t max_size{10 * 1024 * 1024};
    std::size_t max_files{7};
    bool rotate_on_open{false};
    bool truncate_on_open{false};
    ErrorHandler err_handler{ylog_detail_errors::make_default_err_handler()};
};

namespace detail {

inline auto validate_rotating_config(RotatingFileConfig config) -> RotatingFileConfig {
    if (config.max_size == 0) {
        fail_rotating_config("max_size must be greater than 0");
    }
    if (config.max_files > 200000) {
        fail_rotating_config(std::format("max_files must be <= 200000 (got {})", config.max_files));
    }
    return config;
}

}  // namespace detail

export class RotatingFileSink {
public:
    explicit RotatingFileSink(RotatingFileConfig config)
        : config_(detail::validate_rotating_config(std::move(config))),
          stream_(detail::file_stream::open(detail::rotating_sink_name,
                                            config_.filename,
                                            config_.truncate_on_open,
                                            detail::fail_rotating_config)),
          current_size_(detail::file_size_or_zero(config_.filename)) {
        if (config_.rotate_on_open && current_size_ > config_.max_size) {
            rotate_();
        }
    }

    RotatingFileSink(const RotatingFileSink&) = delete;
    auto operator=(const RotatingFileSink&) -> RotatingFileSink& = delete;
    RotatingFileSink(RotatingFileSink&&) noexcept = default;
    auto operator=(RotatingFileSink&&) noexcept -> RotatingFileSink& = default;
    ~RotatingFileSink() = default;

    auto write(std::string_view line) -> void {
        if (current_size_ + line.size() > config_.max_size) {
            rotate_();
        }
        if (!stream_.write(line, config_.err_handler)) {
            return;
        }
        current_size_ += line.size();
    }

    auto flush() -> void { stream_.flush(config_.err_handler); }

private:
    auto rotate_() -> void {
        stream_.flush(config_.err_handler, "flush before rotate");
        stream_.close();

        for (std::size_t index = config_.max_files; index > 0; --index) {
            auto source = index == 1 ? std::filesystem::path(config_.filename)
                                     : detail::backup_path(config_.filename, index - 1);
            auto target = detail::backup_path(config_.filename, index);
            auto error = std::error_code{};
            if (index == config_.max_files) {
                std::filesystem::remove(target, error);
                if (error) {
                    detail::report_io_failure(detail::rotating_sink_name, config_.err_handler,
                                              "remove oldest backup", target, error.message());
                    error.clear();
                }
            }
            auto exists_error = std::error_code{};
            if (!std::filesystem::exists(source, exists_error)) {
                continue;
            }
            std::filesystem::rename(source, target, error);
            if (error) {
                detail::report_io_failure(detail::rotating_sink_name, config_.err_handler,
                                          "rotate rename", target, error.message());
                error.clear();
            }
        }

        stream_.reopen(config_.filename, true, config_.err_handler, "reopen after rotate");
        current_size_ = 0;
    }

    RotatingFileConfig config_;
    detail::file_stream stream_;
    std::size_t current_size_{0};
};

}  // namespace ylog
