export module ylog:sink.file.daily;

import std;
import :sink;
import :sink.file.lifecycle;
import ylog_detail_errors;

namespace ylog {

namespace detail {

// 热路径上报文案里的 sink 名（如 "daily file sink write failed for ..."）。
// 配置期前缀是另一套文本，由下面的 fail_daily_config 给出类型名 "DailyFileSink"。
inline constexpr std::string_view daily_sink_name{"daily file sink"};

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
    ErrorHandler err_handler{ylog_detail_errors::make_default_err_handler()};
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
          stream_(detail::file_stream::open(detail::daily_sink_name,
                                            current_path_,
                                            config_.truncate_on_open,
                                            detail::fail_daily_config)),
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
        if (!stream_.write(line, config_.err_handler)) {
            return;
        }
        apply_retention_();
    }

    auto flush() -> void { stream_.flush(config_.err_handler); }

private:
    auto rotate_(std::chrono::system_clock::time_point now) -> void {
        stream_.close();
        current_path_ = detail::daily_path(config_.filename, now);
        current_path_existed_ = detail::path_exists(current_path_);
        stream_.reopen(current_path_, config_.truncate_on_open, config_.err_handler,
                       "open after rotate");
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
                detail::report_io_failure(detail::daily_sink_name, config_.err_handler,
                                          "remove old daily file", victim, error.message());
            }
        }
    }

    DailyFileConfig config_;
    std::function<std::chrono::system_clock::time_point()> clock_;
    std::filesystem::path current_path_;
    bool current_path_existed_{false};
    detail::file_stream stream_;
    std::chrono::system_clock::time_point next_rotation_{};
    std::vector<std::filesystem::path> created_files_;
};

}  // namespace ylog
