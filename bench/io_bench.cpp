// 写盘路径分解基准 —— 回答「差距在 sink 还是 formatter」。
//
// 三层场景，读数之间的差值就是各层成本：
//   1. 原语：ofstream / fwrite（默认缓冲与 1MB 缓冲各一档）、逐条 write(2)；
//   2. 库内：FileSink 的单条 write（不经 formatter），以及 Logger 的整链；
//   3. 端到端：Logger::info，pattern 分 %v 与默认两种，差值即格式化成本。
//
// 行内容固定 34 字节：写盘成本因此只反映调用开销，不含格式化差异。
// 每条基准固定 20 万次迭代（约 6.4 MB 落盘），避免按 min_time 自适应时把
// bench_logs/io 撑到 GB 级。
//
// 用法：bench_io [--benchmark_filter=...]

#include "benchmark/benchmark.h"

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <fstream>

import std;
import ylog;
import bench.support;

namespace {

constexpr auto bench_directory = "bench_logs/io";
constexpr auto iterations = std::int64_t{200'000};
constexpr auto message_line = std::string_view{"Hello logger: msg number 123456\n"};
constexpr auto big_buffer = std::size_t{1024 * 1024};

auto file_path(std::string_view name) -> std::string {
    return bench::log_path(bench_directory, name);
}

auto ofstream_bench(benchmark::State& state, std::size_t buffer_size) -> void {
    auto out = std::ofstream{};
    auto buffer = std::vector<char>{};
    if (buffer_size > 0) {
        buffer.resize(buffer_size);
        out.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    }
    out.open(file_path(buffer_size > 0 ? "ofstream_big" : "ofstream"),
             std::ios::out | std::ios::trunc | std::ios::binary);
    for (auto _ : state) {
        out.write(message_line.data(), static_cast<std::streamsize>(message_line.size()));
    }
    out.flush();
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(message_line.size()));
}

auto fwrite_bench(benchmark::State& state, std::size_t buffer_size) -> void {
    auto* out = std::fopen(file_path(buffer_size > 0 ? "fwrite_big" : "fwrite").c_str(), "wb");
    auto buffer = std::vector<char>{};
    if (buffer_size > 0) {
        buffer.resize(buffer_size);
        std::setvbuf(out, buffer.data(), _IOFBF, buffer.size());
    }
    for (auto _ : state) {
        std::fwrite(message_line.data(), 1, message_line.size(), out);
    }
    std::fclose(out);
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(message_line.size()));
}

#if defined(__linux__)
// 无用户态缓冲的上界参考：每条一次系统调用。
auto syscall_bench(benchmark::State& state) -> void {
    auto fd = ::open(file_path("syscall").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (auto _ : state) {
        ::write(fd, message_line.data(), message_line.size());
    }
    ::close(fd);
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(message_line.size()));
}
#endif

// 库内单条写：不经 formatter，也不持有 Logger。
auto ylog_file_sink_bench(benchmark::State& state) -> void {
    auto sink = ylog::FileSink{
        ylog::FileConfig{.filename = file_path("ylog_file_sink"), .truncate_on_open = true}};
    for (auto _ : state) {
        sink.write(message_line);
    }
    sink.flush();
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(message_line.size()));
}

// AnySink 不可拷贝，无法作为 RegisterBenchmark 的绑定参数（GB 内部按值存进
// std::function），因此三个变体各写一个外壳，在函数体内就地构造 sink。
auto ylog_logger_bench(benchmark::State& state, ylog::AnySink sink) -> void {
    auto logger = ylog::Logger{ylog::LoggerConfig{
        .name = "io",
        .sinks = ylog::make_sinks(std::move(sink)),
    }};
    auto index = 0;
    for (auto _ : state) {
        logger.info({}, "Hello logger: msg number {}", ++index);
    }
    logger.flush();
}

auto bm_logger_null(benchmark::State& state) -> void {
    ylog_logger_bench(state, ylog::AnySink{ylog::NullSink{}, std::string_view{"%v"}});
}

auto bm_logger_file(benchmark::State& state) -> void {
    ylog_logger_bench(state, ylog::AnySink{ylog::FileSink{ylog::FileConfig{
                                               .filename = file_path("ylog_logger_v"),
                                               .truncate_on_open = true,
                                           }},
                                           std::string_view{"%v"}});
}

// 默认 pattern（带时间戳、级别、名称）：与上一行的差值即格式化成本。
auto bm_logger_file_default_pattern(benchmark::State& state) -> void {
    ylog_logger_bench(state, ylog::AnySink{ylog::FileSink{ylog::FileConfig{
                                 .filename = file_path("ylog_logger_default"),
                                 .truncate_on_open = true,
                             }}});
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        bench::prepare_directory(bench_directory);

        benchmark::RegisterBenchmark("ofstream", ofstream_bench, std::size_t{0})
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);
        benchmark::RegisterBenchmark("ofstream/1MB-buffer", ofstream_bench, big_buffer)
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);
        benchmark::RegisterBenchmark("fwrite", fwrite_bench, std::size_t{0})
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);
        benchmark::RegisterBenchmark("fwrite/1MB-buffer", fwrite_bench, big_buffer)
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);
#if defined(__linux__)
        benchmark::RegisterBenchmark("write(2)", syscall_bench)
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);
#endif

        benchmark::RegisterBenchmark("ylog/file_sink", ylog_file_sink_bench)
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);

        benchmark::RegisterBenchmark("ylog/logger_null/%v", bm_logger_null)
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);
        benchmark::RegisterBenchmark("ylog/logger_file/%v", bm_logger_file)
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);
        benchmark::RegisterBenchmark("ylog/logger_file/default-pattern",
                                     bm_logger_file_default_pattern)
            ->Iterations(iterations)
            ->Unit(benchmark::kNanosecond);

        benchmark::Initialize(&argc, argv);
        benchmark::RunSpecifiedBenchmarks();
    } catch (const std::exception& error) {
        std::println(std::cerr, "错误: {}", error.what());
        return 1;  // EXIT_FAILURE 是宏，import std 带不进来
    }
    return 0;
}
