// formatter 基准 —— 对应 spdlog 1.17.0 的 bench/formatter-bench.cpp。
//
// 单个 pattern flag 与整条 pattern 的格式化耗时，不经过任何 sink。
//   bench_formatter all          # 跑全部 flag 与常用整串 pattern
//   bench_formatter "%Y-%m-%d"   # 只跑指定 pattern
//
// flag 集合取自 :formatter 的 make_flag 分支（ylog 支持的全集），与 spdlog 的交集
// 之外还有 % g s # ! u i o O & 等。

#include "benchmark/benchmark.h"

import std;
import ylog;

namespace {

void bench_formatter(benchmark::State& state, std::string pattern) {
    auto formatter = ylog::PatternFormatter{pattern};
    auto dest = std::string{};
    dest.reserve(256);

    // 与 spdlog 一致：消息只构造一次，循环里只付格式化成本。
    auto location = ylog::SourceLocation{"a/b/c/d/myfile.cpp", 123, "some_func()"};
    auto message = ylog::Message{
        ylog::Level::Info, "logger-name", location,
        "Hello. This is some message with length of 80                                   "};

    for (auto _ : state) {
        dest.clear();
        formatter.format(message, dest);
        benchmark::DoNotOptimize(dest);
    }
}

auto bench_all() -> void {
    // 单 flag：每个 flag 一条基准，名字即 pattern 本身。
    constexpr auto all_flags = std::string_view{
        "+vtPnlLaAbhBcCyYDxmdHIMSp"
        "rRTXzefFE@gs#!uioO&^$%"};
    for (auto flag : all_flags) {
        auto pattern = std::format("%{}", flag);
        benchmark::RegisterBenchmark(pattern.c_str(), bench_formatter, pattern);
    }

    // 常用整串 pattern：默认 pattern、spdlog 的三种组合，以及最重的全字段组合。
    const auto patterns = std::vector<std::string>{
        std::string{ylog::default_pattern},           "[%D %X] [%l] [%n] %v",
        "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v",        "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [%t] %v",
        "[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:#] %v",
    };
    for (const auto& pattern : patterns) {
        benchmark::RegisterBenchmark(pattern.c_str(), bench_formatter, pattern)
            ->Iterations(1'000'000);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println(std::cerr, "用法: {} <pattern>（\"all\" 跑全部）", argv[0]);
        return 1;  // EXIT_FAILURE 是宏，import std 带不进来
    }

    try {
        auto pattern = std::string{argv[1]};
        if (pattern == "all") {
            bench_all();
        } else {
            benchmark::RegisterBenchmark(pattern.c_str(), bench_formatter, pattern);
        }

        benchmark::Initialize(&argc, argv);
        benchmark::RunSpecifiedBenchmarks();
    } catch (const std::exception& error) {
        std::println(std::cerr, "错误: {}", error.what());
        return 1;
    }
    return 0;
}
