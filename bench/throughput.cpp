// 吞吐基准 —— 对应 spdlog 1.17.0 的 bench/bench.cpp。
//
// 单线程与多线程下各类 sink 的消息吞吐（条/秒）。用法：
//   bench_throughput [迭代数] [线程数]
//
// 与 spdlog 的差异：spdlog 的 logger 可以只挂一个 null sink，ylog 的组件构造时
// 自带 StdoutSink 且没有摘除接口，所以这里统一换成待测 sink（见
// bench::replace_default_sink）。backtrace 场景 ylog 没有提供，不设。

import std;
import ylog;
import bench.support;

namespace {

constexpr auto bench_directory = "bench_logs";
constexpr auto rotating_size = std::size_t{30 * 1024 * 1024};
constexpr auto rotating_files = std::size_t{5};
constexpr auto max_threads = std::size_t{1000};

auto file_sink(std::string_view name) -> ylog::AnySink {
    return ylog::AnySink{ylog::FileSink{bench::log_path(bench_directory, name)}};
}

auto rotating_sink(std::string_view name) -> ylog::AnySink {
    return ylog::AnySink{ylog::RotatingFileSink{ylog::RotatingFileConfig{
        .filename = bench::log_path(bench_directory, name),
        .max_size = rotating_size,
        .max_files = rotating_files,
    }}};
}

auto daily_sink(std::string_view name) -> ylog::AnySink {
    return ylog::AnySink{ylog::DailyFileSink{
        ylog::DailyFileConfig{.filename = bench::log_path(bench_directory, name)}}};
}

// 单线程：跑 iterations 条。
auto bench_single(std::string name, std::size_t iterations, ylog::Logger& logger) -> void {
    auto result = bench::timed(std::move(name), iterations, [&] {
        for (auto i = std::size_t{0}; i < iterations; ++i) {
            logger.info({}, "Hello logger: msg number {}", i);
        }
    });
    bench::print(result);
}

// 多线程：threads 个线程共享同一个 logger，每个线程发 iterations / threads 条。
auto bench_multi(std::string name,
                 std::size_t iterations,
                 ylog::Logger& logger,
                 std::size_t threads) -> void {
    auto per_thread = iterations / threads;
    auto result = bench::timed(std::move(name), per_thread * threads, [&] {
        auto workers = std::vector<std::thread>{};
        workers.reserve(threads);
        for (auto index = std::size_t{0}; index < threads; ++index) {
            workers.emplace_back([&logger, per_thread] {
                for (auto i = std::size_t{0}; i < per_thread; ++i) {
                    logger.info({}, "Hello logger: msg number {}", i);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    });
    bench::print(result);
}

auto single_threaded(std::size_t iterations) -> void {
    bench::print_header(std::format("单线程：{} 条消息", iterations));

    {
        auto logger = bench::make_logger("null_st", bench::null_sink());
        bench_single("null_st", iterations, *logger);
    }
    {
        auto logger = bench::make_logger("file_st", file_sink("file_st"));
        bench_single("file_st", iterations, *logger);
    }
    {
        auto logger = bench::make_logger("rotating_st", rotating_sink("rotating_st"));
        bench_single("rotating_st", iterations, *logger);
    }
    {
        auto logger = bench::make_logger("daily_st", daily_sink("daily_st"));
        bench_single("daily_st", iterations, *logger);
    }
    {
        auto logger = bench::make_logger("level-off", bench::null_sink(), ylog::Level::Off);
        bench_single("level-off", iterations, *logger);
    }
}

auto multi_threaded(std::size_t threads, std::size_t iterations) -> void {
    bench::print_header(std::format("多线程：{} 个线程，每条 {} 条消息", threads, iterations));

    {
        auto logger = bench::make_logger("file_mt", file_sink("file_mt"));
        bench_multi("file_mt", iterations, *logger, threads);
    }
    {
        auto logger = bench::make_logger("rotating_mt", rotating_sink("rotating_mt"));
        bench_multi("rotating_mt", iterations, *logger, threads);
    }
    {
        auto logger = bench::make_logger("daily_mt", daily_sink("daily_mt"));
        bench_multi("daily_mt", iterations, *logger, threads);
    }
    {
        auto logger = bench::make_logger("level-off", bench::null_sink(), ylog::Level::Off);
        bench_multi("level-off", iterations, *logger, threads);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    auto iterations = std::size_t{250'000};
    auto threads = std::size_t{4};

    try {
        if (argc > 1) {
            iterations = static_cast<std::size_t>(std::stoul(argv[1]));
        }
        if (argc > 2) {
            threads = static_cast<std::size_t>(std::stoul(argv[2]));
        }
        if (threads == 0) {
            throw std::runtime_error{"线程数必须大于 0"};
        }
        if (threads > max_threads) {
            throw std::runtime_error{std::format("线程数超过上限（{}）", max_threads)};
        }

        bench::prepare_directory(bench_directory);
        single_threaded(iterations);
        multi_threaded(1, iterations);
        multi_threaded(threads, iterations);
    } catch (const std::exception& error) {
        std::println(std::cerr, "错误: {}", error.what());
        return 1;  // EXIT_FAILURE 是宏，import std 带不进来
    }
    return 0;
}
