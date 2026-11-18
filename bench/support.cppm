// 基准程序的公共支撑：计时、结果输出、组件构造与文件校验。
//
// 只放四个程序都要用的东西。各程序的度量场景按 spdlog 1.17.0 的 bench/ 对齐，
// 差异（无 backtrace、有 OverflowPolicy 三态）在各自文件顶部说明。

export module bench.support;

import std;
import ylog;

export namespace bench {

// 一次计时的结果：消息条数 + 墙钟秒数。
struct Measurement {
    std::string name;
    std::size_t messages{0};
    double seconds{0.0};
};

inline auto per_second(const Measurement& result) -> double {
    return result.seconds > 0.0 ? static_cast<double>(result.messages) / result.seconds : 0.0;
}

inline auto print_header(std::string_view title) -> void {
    std::println("**************************************************************");
    std::println("{}", title);
    std::println("**************************************************************");
}

inline auto print(const Measurement& result) -> void {
    std::println("{:<34} Elapsed: {:>8.3f} secs {:>14.0f}/sec", result.name, result.seconds,
                 per_second(result));
}

// 计时：fn() 执行一次，messages 是它产生的日志条数。
template <typename Fn>
auto timed(std::string name, std::size_t messages, Fn&& fn) -> Measurement {
    auto start = std::chrono::steady_clock::now();
    std::invoke(std::forward<Fn>(fn));
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return Measurement{.name = std::move(name), .messages = messages, .seconds = seconds};
}

// 最常用的替身：NullSink + 指定 pattern。
inline auto null_sink(std::string_view pattern = "%v") -> ylog::AnySink {
    return ylog::AnySink{ylog::NullSink{}, std::make_unique<ylog::PatternFormatter>(pattern)};
}

// 同步组件工厂：sink 集合在构造期给定，组件不再自带 stdout sink。
// Logger 持原子成员，不可移动，故用 unique_ptr。
inline auto make_logger(std::string name, ylog::AnySink sink, ylog::Level level = ylog::Level::Info)
    -> std::unique_ptr<ylog::Logger> {
    return std::make_unique<ylog::Logger>(ylog::LoggerConfig{
        .name = std::move(name),
        .level = level,
        .sinks = ylog::make_sinks(std::move(sink)),
    });
}

// 异步组件工厂：同样在构造期给定 sink。
inline auto make_async_logger(std::string name,
                              ylog::AnySink sink,
                              std::size_t queue_capacity,
                              ylog::OverflowPolicy policy = ylog::OverflowPolicy::Block)
    -> std::unique_ptr<ylog::AsyncLogger> {
    return std::make_unique<ylog::AsyncLogger>(ylog::AsyncLoggerConfig{
        .name = std::move(name),
        .queue_capacity = queue_capacity,
        .overflow_policy = policy,
        .sinks = ylog::make_sinks(std::move(sink)),
    });
}

// 基准日志一律落在 bench_logs/ 下，避免污染源码树。
inline auto log_path(std::string_view directory, std::string_view name) -> std::string {
    return std::format("{}/{}.log", directory, name);
}

inline auto prepare_directory(const std::filesystem::path& path) -> void {
    auto error = std::error_code{};
    std::filesystem::create_directories(path, error);
    if (error) {
        throw std::runtime_error{
            std::format("无法创建目录 '{}': {}", path.string(), error.message())};
    }
}

// 行数统计：异步基准用它校验「Block 策略下一条不丢」。
inline auto count_lines(const std::filesystem::path& path) -> std::size_t {
    auto stream = std::ifstream{path};
    if (!stream) {
        throw std::runtime_error{std::format("无法读取 '{}'", path.string())};
    }
    auto content =
        std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    return static_cast<std::size_t>(std::ranges::count(content, '\n'));
}

}  // namespace bench
