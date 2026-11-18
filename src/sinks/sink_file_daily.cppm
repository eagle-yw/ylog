export module ylog:sink.file.daily;

import std;
import :sink;
import ylog_detail_errors;

namespace ylog {

namespace detail {

inline auto fail_daily_config(std::string_view reason) -> void {
    ylog_detail_errors::fail_config(std::format("DailyFileSink: {}", reason));
}

inline auto daily_localtime(std::time_t seconds) -> std::tm {
    static std::mutex mutex;
    auto lock = std::lock_guard(mutex);
    auto result = std::tm{};
    if (auto* value = std::localtime(&seconds); value != nullptr) {
        result = *value;
    }
    return result;
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

inline auto path_exists(const std::filesystem::path& path) -> bool {
    auto error = std::error_code{};
    auto result = std::filesystem::exists(path, error);
    return error ? false : result;
}

inline auto try_open_daily_stream(const std::filesystem::path& path,
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

inline auto open_daily_stream(const std::filesystem::path& path, bool truncate) -> std::ofstream {
    auto error = std::error_code{};
    auto stream = try_open_daily_stream(path, truncate, error);
    if (error) {
        fail_daily_config(std::format("failed to open '{}': {}", path.string(), error.message()));
    }
    return stream;
}

inline auto daily_path(const std::string& filename, std::chrono::system_clock::time_point now)
    -> std::filesystem::path {
    auto path = std::filesystem::path(filename);
    auto [stem, extension] = split_stem_and_extension(filename);
    auto value = daily_localtime(std::chrono::system_clock::to_time_t(now));
    auto date =
        std::format("{:04d}-{:02d}-{:02d}", value.tm_year + 1900, value.tm_mon + 1, value.tm_mday);
    return path.parent_path() / std::format("{}_{}{}", stem, date, extension);
}

inline auto next_midnight(std::chrono::system_clock::time_point now)
    -> std::chrono::system_clock::time_point {
    auto value = daily_localtime(std::chrono::system_clock::to_time_t(now));
    value.tm_hour = 0;
    value.tm_min = 0;
    value.tm_sec = 0;
    value.tm_isdst = -1;
    auto candidate = std::chrono::system_clock::from_time_t(std::mktime(&value));
    if (candidate <= now) {
        auto next = value;
        next.tm_mday += 1;
        next.tm_isdst = -1;
        candidate = std::chrono::system_clock::from_time_t(std::mktime(&next));
    }
    return candidate;
}

}  // namespace detail

export struct DailyFileConfig {
    std::string filename;
    std::size_t max_files{7};
    bool truncate_on_open{false};
    std::function<void(std::string_view)> err_handler{
        ylog_detail_errors::make_default_err_handler()};
    std::function<std::chrono::system_clock::time_point()> clock{std::chrono::system_clock::now};
};

namespace detail {

inline auto validate_daily_config(DailyFileConfig config) -> DailyFileConfig {
    if (config.max_files > 200000) {
        fail_daily_config(std::format("max_files must be <= 200000 (got {})", config.max_files));
    }
    if (!config.clock) {
        fail_daily_config("clock must be callable");
    }
    return config;
}

}  // namespace detail

export class DailyFileSink {
public:
    explicit DailyFileSink(DailyFileConfig config)
        : config_(detail::validate_daily_config(std::move(config))),
          clock_(config_.clock),
          current_path_(detail::daily_path(config_.filename, clock_())),
          current_path_existed_(detail::path_exists(current_path_)),
          stream_(detail::open_daily_stream(current_path_, config_.truncate_on_open)),
          next_rotation_(detail::next_midnight(clock_())) {
        if (!current_path_existed_) {
            created_files_.push_back(current_path_);
        }
    }

    DailyFileSink(const DailyFileSink&) = delete;
    auto operator=(const DailyFileSink&) -> DailyFileSink& = delete;
    DailyFileSink(DailyFileSink&&) noexcept = default;
    auto operator=(DailyFileSink&&) noexcept -> DailyFileSink& = default;
    ~DailyFileSink() = default;

    auto write(std::string_view line) -> void {
        auto now = clock_();
        if (now >= next_rotation_) {
            rotate_(now);
        }
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!stream_) {
            report_failure_("write", current_path_, "stream error");
            stream_.clear();
            return;
        }
        apply_retention_();
    }

    auto flush() -> void {
        stream_.flush();
        if (!stream_) {
            report_failure_("flush", current_path_, "stream error");
            stream_.clear();
        }
    }

private:
    auto rotate_(std::chrono::system_clock::time_point now) -> void {
        stream_.close();
        current_path_ = detail::daily_path(config_.filename, now);
        current_path_existed_ = detail::path_exists(current_path_);
        auto error = std::error_code{};
        stream_ = detail::try_open_daily_stream(current_path_, config_.truncate_on_open, error);
        if (error) {
            report_failure_("open after rotate", current_path_, error.message());
            stream_.clear();
        }
        next_rotation_ = detail::next_midnight(now);
        if (!current_path_existed_) {
            created_files_.push_back(current_path_);
        }
    }

    auto apply_retention_() -> void {
        if (config_.max_files == 0) {
            return;
        }
        while (created_files_.size() > config_.max_files) {
            auto victim = created_files_.front();
            created_files_.erase(created_files_.begin());
            auto error = std::error_code{};
            std::filesystem::remove(victim, error);
            if (error) {
                report_failure_("remove old daily file", victim, error.message());
            }
        }
    }

    auto report_failure_(std::string_view operation,
                         const std::filesystem::path& path,
                         std::string_view reason) -> void {
        auto message =
            std::format("daily file sink {} failed for '{}': {}", operation, path.string(), reason);
        ylog_detail_errors::safe_err_handler(config_.err_handler, message);
    }

    DailyFileConfig config_;
    std::function<std::chrono::system_clock::time_point()> clock_;
    std::filesystem::path current_path_;
    bool current_path_existed_{false};
    std::ofstream stream_;
    std::chrono::system_clock::time_point next_rotation_{};
    std::vector<std::filesystem::path> created_files_;
};

}  // namespace ylog
