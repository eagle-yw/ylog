// 异步队列基准 —— 对应 spdlog 1.17.0 的 bench/async_bench.cpp。
//
// 多线程生产者向异步组件入队的吞吐，并校验溢出策略的可见后果：
//   - Block：一条不丢，行数必须等于入队条数；
//   - OverrunOldest：队列满时覆盖最旧，行数只少不多。
// 用法：bench_async [消息数] [线程数] [队列容量] [迭代次数]
//
// pattern 与其它基准统一为 "%v"：本程序量的是入队与交接，不是格式化。
// 用默认 pattern 时消费者要多付时间戳格式化的成本，队满阻塞的次数随之变化，
// 数字会混进 formatter 的差价。

import std;
import ylog;
import bench.support;

namespace {

constexpr auto bench_directory = "bench_logs";
constexpr auto max_queue_capacity = std::size_t{500'000};

struct Options {
    std::size_t messages{1'000'000};
    std::size_t threads{10};
    std::size_t queue_capacity{8192};
    std::size_t iterations{3};
};

auto produce(ylog::AsyncLogger& logger, std::size_t per_thread) -> void {
    for (auto i = std::size_t{0}; i < per_thread; ++i) {
        logger.info({}, "Hello logger: msg number {}", i);
    }
}

auto run_round(const Options& options, std::string_view name, ylog::OverflowPolicy policy) -> void {
    auto logger = bench::make_async_logger(
        std::string{name},
        ylog::AnySink{ylog::FileSink{ylog::FileConfig{
                          .filename = bench::log_path(bench_directory, name),
                          .truncate_on_open = true,
                      }},
                      "%v"},
        options.queue_capacity, policy);

    auto per_thread = options.messages / options.threads;
    auto expected = per_thread * options.threads;
    auto result = bench::timed(std::string{name}, expected, [&] {
        auto workers = std::vector<std::thread>{};
        workers.reserve(options.threads);
        for (auto index = std::size_t{0}; index < options.threads; ++index) {
            workers.emplace_back(produce, std::ref(*logger), per_thread);
        }
        for (auto& worker : workers) {
            worker.join();
        }
    });

    // 屏障：返回时先前入队的消息都已写出，且所有 sink 已 flush 到磁盘。
    logger->flush();
    logger->stop();
    bench::print(result);

    auto lines = bench::count_lines(bench::log_path(bench_directory, name));
    if (policy == ylog::OverflowPolicy::Block) {
        if (lines != expected) {
            throw std::runtime_error{
                std::format("{}: 期望 {} 行，实际 {} 行", name, expected, lines)};
        }
        std::println("{:<34} 行数校验通过：{} 行", name, lines);
    } else if (lines > expected) {
        throw std::runtime_error{std::format("{}: 行数 {} 超过入队条数 {}", name, lines, expected)};
    } else {
        std::println("{:<34} 写出 {} / {} 行，覆盖 {} 条", name, lines, expected, expected - lines);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    auto options = Options{};

    try {
        if (argc > 1) {
            options.messages = static_cast<std::size_t>(std::stoul(argv[1]));
        }
        if (argc > 2) {
            options.threads = static_cast<std::size_t>(std::stoul(argv[2]));
        }
        if (argc > 3) {
            options.queue_capacity = static_cast<std::size_t>(std::stoul(argv[3]));
        }
        if (argc > 4) {
            options.iterations = static_cast<std::size_t>(std::stoul(argv[4]));
        }
        if (options.threads == 0 || options.iterations == 0) {
            throw std::runtime_error{"线程数与迭代次数必须大于 0"};
        }
        if (options.queue_capacity == 0 || options.queue_capacity > max_queue_capacity) {
            throw std::runtime_error{std::format("队列容量须在 1..{} 之间", max_queue_capacity)};
        }

        bench::prepare_directory(bench_directory);
        std::println("-------------------------------------------------");
        std::println("消息条数   : {}", options.messages);
        std::println("线程数     : {}", options.threads);
        std::println("队列容量   : {} 槽", options.queue_capacity);
        std::println("迭代次数   : {}", options.iterations);
        std::println("-------------------------------------------------");

        bench::print_header("队列溢出策略：Block（生产者等待空位，不丢消息）");
        for (auto index = std::size_t{0}; index < options.iterations; ++index) {
            run_round(options, "async_block", ylog::OverflowPolicy::Block);
        }

        bench::print_header("队列溢出策略：OverrunOldest（覆盖最旧，不阻塞生产者）");
        for (auto index = std::size_t{0}; index < options.iterations; ++index) {
            run_round(options, "async_overrun", ylog::OverflowPolicy::OverrunOldest);
        }
    } catch (const std::exception& error) {
        std::println(std::cerr, "错误: {}", error.what());
        return 1;  // EXIT_FAILURE 是宏，import std 带不进来
    }
    return 0;
}
