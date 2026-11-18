// 两个轮转 sink 共用的文件生命周期：路径拆分、父目录创建、流打开与热路径的
// 写/刷失败上报。实现分区、且不导出任何名字：同一模块的 :sink.file.daily 与
// :sink.file.rotating 能用它，模块外既看不见也命名不了（同 :async.queue）。
//
// 这里只放「一个追加/截断的二进制输出流怎么活」与「失败怎么报」；轮转触发条件、
// 文件命名与保留策略留在各自 sink，见 ADR-0005。
module ylog:sink.file.lifecycle;

import std;
import :sink;
import ylog_detail_errors;

namespace ylog {

namespace detail {

// 文件名拆成「主干 + 扩展名」。前导点是主干的一部分（`.log` 没有扩展名），
// 两种命名规则（`_YYYY-MM-DD` 与 `.N`）都靠它拼目标路径。
inline auto split_stem_and_extension(const std::string& filename)
    -> std::pair<std::string, std::string> {
    auto name = std::filesystem::path(filename).filename().string();
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && dot + 1 < name.size()) {
        return {name.substr(0, dot), name.substr(dot)};
    }
    return {name, {}};
}

// 路径存在性：问不出来就算不存在，调用方不需要错误分支。
inline auto path_exists(const std::filesystem::path& path) -> bool {
    auto error = std::error_code{};
    auto result = std::filesystem::exists(path, error);
    return error ? false : result;
}

// 文件大小：问不出来就按 0 处理（首次写入会重新长出来）。
inline auto file_size_or_zero(const std::filesystem::path& path) -> std::size_t {
    auto error = std::error_code{};
    auto size = std::filesystem::file_size(path, error);
    return error ? std::size_t{0} : static_cast<std::size_t>(size);
}

// 热路径失败的唯一文案出口：sink 名 + 操作 + 路径 + 原因。
// 流失败与 sink 自己的文件系统失败（删旧文件、改备份名）共用它。
// handler 由调用方按次传入而不是在构造期抓指针：sink 是可按值移动的，
// 抓 config_ 成员地址会在移动后变成悬垂。
inline auto report_io_failure(std::string_view sink,
                              const ErrorHandler& handler,
                              std::string_view operation,
                              const std::filesystem::path& path,
                              std::string_view reason) -> void {
    auto message = std::format("{} {} failed for '{}': {}", sink, operation, path.string(), reason);
    ylog_detail_errors::safe_err_handler(handler, message);
}

// 配置期失败的出口：各 sink 的 fail_*_config，负责加自己的类型前缀。
using config_failure = void (*)(std::string_view);

// 打开流：递归创建父目录，按 truncate 选 trunc / app，二进制模式。
// 失败经 error 返回而不是抛——配置期与热路径对同一个失败有两种处置。
// 已知偏差（与合并前一致）：父目录建不出来时也报成「打开文件失败」，
// 路径给的是文件而不是父目录。
inline auto try_open_stream(const std::filesystem::path& path,
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

// 一个 sink 的输出流，连同它在上报里的名字。写与刷的失败处置固定为一处：
// 清掉流状态，让下一次写入仍有机会恢复，然后上报一次——两个轮转 sink 都不设
// failure_latch，连续失败逐条上报（与 FileSink 的锁存不同）。
class file_stream {
public:
    // 配置期打开：失败交给调用方的配置期出口（各 sink 的 fail_*_config），
    // 打开失败与校验失败因此共用同一个类型前缀，而不是热路径里的 sink 名。
    static auto open(std::string_view sink,
                     const std::filesystem::path& path,
                     bool truncate,
                     config_failure on_config_failure) -> file_stream {
        auto result = file_stream(sink);
        result.path_ = path;
        auto error = std::error_code{};
        result.stream_ = try_open_stream(path, truncate, error);
        if (error) {
            on_config_failure(
                std::format("failed to open '{}': {}", path.string(), error.message()));
        }
        return result;
    }

    file_stream(const file_stream&) = delete;
    auto operator=(const file_stream&) -> file_stream& = delete;
    file_stream(file_stream&&) noexcept = default;
    auto operator=(file_stream&&) noexcept -> file_stream& = default;
    ~file_stream() = default;

    // 热路径重开：失败只上报并返回 false，调用方继续工作（流保持关闭，
    // 下一次写入会再次失败、再次上报）。旧流由调用方先 close()——轮转要在
    // 重开之前完成改名与删文件，顺序不能收进这里。
    auto reopen(const std::filesystem::path& path,
                bool truncate,
                const ErrorHandler& handler,
                std::string_view operation) -> bool {
        path_ = path;
        auto error = std::error_code{};
        stream_ = try_open_stream(path, truncate, error);
        if (!error) {
            return true;
        }
        stream_.clear();
        report_io_failure(sink_, handler, operation, path, error.message());
        return false;
    }

    // 写一行；失败时上报并返回 false，调用方据此跳过与写入绑定的后续动作
    // （大小累加、按日保留）。
    auto write(std::string_view line, const ErrorHandler& handler) -> bool {
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (stream_) {
            return true;
        }
        stream_.clear();
        report_io_failure(sink_, handler, "write", path_, "stream error");
        return false;
    }

    // 落盘；失败时上报并返回 false。轮转前的 flush 不看结果：关掉旧文件、
    // 改名、重开这条链要继续走到底。
    auto flush(const ErrorHandler& handler, std::string_view operation = "flush") -> bool {
        stream_.flush();
        if (stream_) {
            return true;
        }
        stream_.clear();
        report_io_failure(sink_, handler, operation, path_, "stream error");
        return false;
    }

    auto close() -> void { stream_.close(); }

private:
    explicit file_stream(std::string_view sink)
        : sink_(sink) {}

    std::string_view sink_;  // 字面量，指向静态存储
    std::filesystem::path path_;
    std::ofstream stream_;
};

}  // namespace detail

}  // namespace ylog
