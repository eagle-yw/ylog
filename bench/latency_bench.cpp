// 延迟基准 —— 对应 spdlog 1.17.0 的 bench/latency.cpp。
//
// 用 Google Benchmark 量单条日志的耗时（含迭代次数自适应、统计波动提示）。
//   bench_latency                      # 内存 / /dev/null / 异步场景
//   bench_latency full                 # 追加文件 / 轮转 / 按日 sink 与多线程变体
//   bench_latency --benchmark_filter=null_sink_st
//
// 与 spdlog 的差异：ylog 没有 backtrace 与编译期关闭的宏，故缺少这两类场景；
// 运行期关闭（Level::Off）保留，它量的是 should_log 的守门成本。

#include "benchmark/benchmark.h"

import std;
import ylog;
import bench.support;

namespace {

constexpr auto bench_directory = "bench_logs";
constexpr auto rotating_size = std::size_t{30 * 1024 * 1024};
constexpr auto rotating_files = std::size_t{5};
constexpr auto async_queue_capacity = std::size_t{1024 * 1024};

// 约 500 字节的长消息：量格式化与拷贝成本。
constexpr auto long_message = std::string_view{
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Vestibulum pharetra metus cursus "
    "lacus placerat congue. Nulla egestas, mauris a tincidunt tempus, enim lectus volutpat mi, "
    "eu consequat sem libero nec massa. In dapibus ipsum a diam rhoncus gravida. Etiam non "
    "dapibus eros. Donec fringilla dui sed augue pretium, nec scelerisque est maximus. Nullam "
    "convallis, sem nec blandit maximus, nisi turpis ornare nisl, sit amet volutpat neque massa "
    "eu odio. Maecenas malesuada quam ex, posuere congue nibh turpis duis."};

// 注册的 logger 必须活到 RunSpecifiedBenchmarks 之后，这里统一托管所有权：
// 所有 make_logger 的结果都推进 owners，注册时只交出裸指针。
using Owners = std::vector<std::unique_ptr<ylog::Logger>>;

auto keep(Owners& owners, std::unique_ptr<ylog::Logger> logger) -> ylog::Logger* {
    owners.push_back(std::move(logger));
    return owners.back().get();
}

template <typename Component>
void bench_logger(benchmark::State& state, Component* logger) {
    auto i = 0;
    for (auto _ : state) {
        logger->info({}, "Hello logger: msg number {}...............", ++i);
    }
}

template <typename Component>
void bench_long_message(benchmark::State& state, Component* logger) {
    for (auto _ : state) {
        logger->info({}, "{}", long_message);
    }
}

// 全局 logger：多一次常驻槽位查找；级别在计时循环外设置。
void bench_global_logger(benchmark::State& state, ylog::Level level) {
    ylog::set_level(level);
    auto i = 0;
    for (auto _ : state) {
        ylog::info({}, "Hello logger: msg number {}...............", ++i);
    }
}

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

// 文件类场景：真实 I/O，必须用墙钟时间。
auto register_file_benchmarks(Owners& owners, std::size_t threads) -> void {
    auto thread_count = static_cast<int>(threads);

    auto* file_st = keep(owners, bench::make_logger("file_st", file_sink("file_st")));
    benchmark::RegisterBenchmark("file_st", bench_logger<ylog::Logger>, file_st)
        ->UseRealTime()
        ->Unit(benchmark::kMicrosecond);

    auto* rotating_st =
        keep(owners, bench::make_logger("rotating_st", rotating_sink("rotating_st")));
    benchmark::RegisterBenchmark("rotating_st", bench_logger<ylog::Logger>, rotating_st)
        ->UseRealTime()
        ->Unit(benchmark::kMicrosecond);

    auto* daily_st = keep(owners, bench::make_logger("daily_st", daily_sink("daily_st")));
    benchmark::RegisterBenchmark("daily_st", bench_logger<ylog::Logger>, daily_st)
        ->UseRealTime()
        ->Unit(benchmark::kMicrosecond);

    auto* file_mt = keep(owners, bench::make_logger("file_mt", file_sink("file_mt")));
    benchmark::RegisterBenchmark("file_mt", bench_logger<ylog::Logger>, file_mt)
        ->Threads(thread_count)
        ->UseRealTime()
        ->Unit(benchmark::kMicrosecond);

    auto* rotating_mt =
        keep(owners, bench::make_logger("rotating_mt", rotating_sink("rotating_mt")));
    benchmark::RegisterBenchmark("rotating_mt", bench_logger<ylog::Logger>, rotating_mt)
        ->Threads(thread_count)
        ->UseRealTime()
        ->Unit(benchmark::kMicrosecond);

    auto* daily_mt = keep(owners, bench::make_logger("daily_mt", daily_sink("daily_mt")));
    benchmark::RegisterBenchmark("daily_mt", bench_logger<ylog::Logger>, daily_mt)
        ->Threads(thread_count)
        ->UseRealTime()
        ->Unit(benchmark::kMicrosecond);
}

}  // namespace

int main(int argc, char* argv[]) {
    auto full = argc > 1 && std::string_view{argv[1]} == "full";
    auto threads = static_cast<std::size_t>(benchmark::CPUInfo::Get().num_cpus);

    try {
        bench::prepare_directory(bench_directory);

        // 所有同步 logger 的所有权集中在这里：注册的指针对它们取址，必须活到基准跑完。
        auto owners = Owners{};
        owners.reserve(16);

        // 运行期关闭：只有 should_log 的守门成本。
        auto* disabled =
            keep(owners, bench::make_logger("disabled", bench::null_sink(), ylog::Level::Off));
        benchmark::RegisterBenchmark("disabled-at-runtime", bench_logger<ylog::Logger>, disabled)
            ->Unit(benchmark::kMicrosecond);

        // 全局 logger 的两态：默认 sink 换成 NullSink，级别在循环外由被测函数设置。
        bench::replace_default_sink(ylog::default_logger(), bench::null_sink());
        benchmark::RegisterBenchmark("disabled-at-runtime (global logger)", bench_global_logger,
                                     ylog::Level::Off)
            ->Unit(benchmark::kMicrosecond);
        benchmark::RegisterBenchmark("null_sink_st (global logger)", bench_global_logger,
                                     ylog::Level::Info)
            ->Unit(benchmark::kMicrosecond);

        auto* null_st = keep(owners, bench::make_logger("null_st", bench::null_sink()));
        benchmark::RegisterBenchmark("null_sink_st", bench_logger<ylog::Logger>, null_st)
            ->Unit(benchmark::kMicrosecond);
        benchmark::RegisterBenchmark("null_sink_st (long message)",
                                     bench_long_message<ylog::Logger>, null_st)
            ->Unit(benchmark::kMicrosecond);

        auto* null_mt = keep(owners, bench::make_logger("null_mt", bench::null_sink()));
        benchmark::RegisterBenchmark("null_sink_mt", bench_logger<ylog::Logger>, null_mt)
            ->Threads(static_cast<int>(threads))
            ->UseRealTime()
            ->Unit(benchmark::kMicrosecond);

#if defined(__linux__)
        // 文件系统开销的对照组：写 /dev/null 仍有 write(2)，但没有落盘。
        auto* dev_null_st = keep(
            owners, bench::make_logger("dev_null_st", ylog::AnySink{ylog::FileSink{"/dev/null"}}));
        benchmark::RegisterBenchmark("/dev/null_st", bench_logger<ylog::Logger>, dev_null_st)
            ->UseRealTime()
            ->Unit(benchmark::kMicrosecond);

        auto* dev_null_mt = keep(
            owners, bench::make_logger("dev_null_mt", ylog::AnySink{ylog::FileSink{"/dev/null"}}));
        benchmark::RegisterBenchmark("/dev/null_mt", bench_logger<ylog::Logger>, dev_null_mt)
            ->Threads(static_cast<int>(threads))
            ->UseRealTime()
            ->Unit(benchmark::kMicrosecond);
#endif

        if (full) {
            register_file_benchmarks(owners, threads);
        }

        // 异步：生产者只入队，队列满时覆盖最旧（不阻塞生产者，量的是入队路径）。
        // 异步组件不可移动，单独持有。
        auto async_logger = bench::make_async_logger(
            "async", bench::null_sink(), async_queue_capacity, ylog::OverflowPolicy::OverrunOldest);
        benchmark::RegisterBenchmark("async_logger", bench_logger<ylog::AsyncLogger>,
                                     async_logger.get())
            ->Threads(static_cast<int>(threads))
            ->UseRealTime()
            ->Unit(benchmark::kMicrosecond);

        benchmark::Initialize(&argc, argv);
        benchmark::RunSpecifiedBenchmarks();

        // 显式停机：让排空发生在报告之后，而不是 main 退出时。
        async_logger->stop();
    } catch (const std::exception& error) {
        std::println(std::cerr, "错误: {}", error.what());
        return 1;  // EXIT_FAILURE 是宏，import std 带不进来
    }
    return 0;
}
