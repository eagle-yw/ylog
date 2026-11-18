export module ylog:sink.file.rotating;

import std;
import :sink;
import ylog_detail_errors;

namespace ylog {

namespace detail {

inline auto fail_rotating_config(std::string_view reason) -> void {
    ylog_detail_errors::fail_config(std::format("RotatingFileSink: {}", reason));
}

inline auto split_stem_and_extension(const std::string& filename)
    -> std::pair<std::string, std::string> {
    auto name = std::filesystem::path(filename).filename().string();
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && dot + 1 < name.size()) {
        return {name.substr(0, dot), name.substr(dot)};
    }
    return {name, {}};
}

inline auto try_open_rotating_stream(const std::filesystem::path& path,
                                     bool truncate,
                                     std::error_code& error) -> std::ofstream {
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return {};
        }
    }
    auto mode = std::ios::out | std::ios::binary | (truncate ? std::ios::trunc : std::ios::app);
    auto stream = std::ofstream(path, mode);
    if (!stream.is_open()) {
        error = std::make_error_code(std::errc::io_error);
        return {};
    }
    return stream;
}

inline auto open_rotating_stream(const std::string& filename, bool truncate) -> std::ofstream {
    auto error = std::error_code{};
    auto stream = try_open_rotating_stream(filename, truncate, error);
    if (error) {
        fail_rotating_config(std::format("failed to open '{}': {}", filename, error.message()));
    }
    return stream;
}

inline auto file_size_or_zero(const std::string& filename) -> std::size_t {
    auto error = std::error_code{};
    auto size = std::filesystem::file_size(filename, error);
    return error ? std::size_t{0} : static_cast<std::size_t>(size);
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
          stream_(detail::open_rotating_stream(config_.filename, config_.truncate_on_open)),
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
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!stream_) {
            report_failure_("write", config_.filename, "stream error");
            stream_.clear();
            return;
        }
        current_size_ += line.size();
    }

    auto flush() -> void {
        stream_.flush();
        if (!stream_) {
            report_failure_("flush", config_.filename, "stream error");
            stream_.clear();
        }
    }

private:
    auto rotate_() -> void {
        stream_.flush();
        if (!stream_) {
            report_failure_("flush before rotate", config_.filename, "stream error");
            stream_.clear();
        }
        stream_.close();

        for (std::size_t index = config_.max_files; index > 0; --index) {
            auto source = index == 1 ? std::filesystem::path(config_.filename)
                                     : detail::backup_path(config_.filename, index - 1);
            auto target = detail::backup_path(config_.filename, index);
            auto error = std::error_code{};
            if (index == config_.max_files) {
                std::filesystem::remove(target, error);
                if (error) {
                    report_failure_("remove oldest backup", target, error.message());
                    error.clear();
                }
            }
            auto exists_error = std::error_code{};
            if (!std::filesystem::exists(source, exists_error)) {
                continue;
            }
            std::filesystem::rename(source, target, error);
            if (error) {
                report_failure_("rotate rename", target, error.message());
                error.clear();
            }
        }

        auto reopen_error = std::error_code{};
        stream_ = detail::try_open_rotating_stream(config_.filename, true, reopen_error);
        if (reopen_error) {
            report_failure_("reopen after rotate", config_.filename, reopen_error.message());
            stream_.clear();
        }
        current_size_ = 0;
    }

    auto report_failure_(std::string_view operation,
                         const std::filesystem::path& path,
                         std::string_view reason) -> void {
        auto message = std::format("rotating file sink {} failed for '{}': {}", operation,
                                   path.string(), reason);
        ylog_detail_errors::safe_err_handler(config_.err_handler, message);
    }

    RotatingFileConfig config_;
    std::ofstream stream_;
    std::size_t current_size_{0};
};

}  // namespace ylog
