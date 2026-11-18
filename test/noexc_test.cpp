// 无异常构建（-fno-exceptions + YLOG_NO_EXCEPTIONS）的独立 ctest target。
// 只覆盖不抛异常的路径：正常配置、级别过滤、异步组件与并发冒烟，
// 以及热路径失败交给 err_handler。
// 不使用 gtest：gtest 的 GTEST_HAS_EXCEPTIONS 口径须与整份库一致，混用会引入 ODR 风险。

import std;
import ylog;
import ylog_detail_errors;

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#error "noexc_test 必须以 -fno-exceptions 编译"
#endif

// 打开异常后配置失败走 throw，因此这里只能是编译期确认：开关确实生效。
static_assert(!ylog_detail_errors::exceptions_enabled);

namespace {

auto failures = int{0};

auto check(bool condition, std::string_view what) -> void {
    if (!condition) {
        std::println(std::cerr, "FAILED: {}", what);
        ++failures;
    }
}

struct Capture {
    std::string* out;
    auto write(std::string_view line) -> void { *out += line; }
};

auto temp_directory() -> std::filesystem::path {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() / std::format("ylog-noexc-{}", stamp);
    std::filesystem::create_directories(path);
    return path;
}

auto read_all(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// 热路径失败必须走 err_handler，而不是 throw / terminate。
auto reports_hot_path_failure() -> void {
    if (!std::filesystem::exists("/dev/full")) {
        std::println("skip: /dev/full unavailable");
        return;
    }

    auto reported = std::string{};
    auto config = ylog::RotatingFileConfig{};
    config.filename = "/dev/full";
    config.err_handler = [&reported](std::string_view message) { reported = std::string(message); };

    auto sink = ylog::RotatingFileSink(config);
    sink.write("hello\n");
    sink.flush();
    check(!reported.empty(), "hot path failure reaches err_handler");
}

// 无异常构建下配置失败是 assert + println + terminate：断言只表达前置条件，
// NDEBUG 下会被编译掉，终止是无条件的。这条路径无法在进程内自检（一旦触发就没了），
// 只能靠文件顶部的 static_assert 确认开关口径；此处不构造非法配置。

// 只解析非负整数：行格式由本文件的生产者决定，不用 std::stoi（它靠异常报错）。
auto parse_non_negative(std::string_view text) -> int {
    if (text.empty()) {
        return -1;
    }
    auto value = 0;
    for (auto ch : text) {
        if (ch < '0' || ch > '9') {
            return -1;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

// 计量 sink：记录每条落盘行的 (生产者, 序号)，用于总量与 FIFO 断言。
struct MeasuredSink {
    struct State {
        std::mutex mutex;
        std::vector<std::pair<int, int>> lines;
    };

    std::shared_ptr<State> state;

    auto write(std::string_view line) -> void {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        auto separator = line.find('-');
        if (separator == std::string_view::npos) {
            return;
        }
        auto producer = parse_non_negative(line.substr(0, separator));
        auto sequence = parse_non_negative(line.substr(separator + 1));
        std::lock_guard lock(state->mutex);
        state->lines.emplace_back(producer, sequence);
    }

    auto flush() -> void {}
};

auto snapshot(MeasuredSink::State& state) -> std::vector<std::pair<int, int>> {
    std::lock_guard lock(state.mutex);
    return state.lines;
}

// 上报计数交给 err_handler：非阻塞策略会从多个生产者线程并发调用它。
struct OverflowReports {
    std::shared_ptr<std::atomic<int>> overrun = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> discarded = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> other = std::make_shared<std::atomic<int>>(0);

    auto handler() const -> ylog::ErrorHandler {
        auto overrun_count = overrun;
        auto discarded_count = discarded;
        auto other_count = other;
        return [overrun_count, discarded_count, other_count](std::string_view message) {
            if (message.find("overrun_oldest") != std::string_view::npos) {
                overrun_count->fetch_add(1, std::memory_order_relaxed);
            } else if (message.find("discard_new") != std::string_view::npos) {
                discarded_count->fetch_add(1, std::memory_order_relaxed);
            } else {
                other_count->fetch_add(1, std::memory_order_relaxed);
            }
        };
    }
};

// 并发冒烟：4 个生产者 × 50 条，队列容量 16 刻意小于产量，
// 让两种非阻塞策略必须丢弃。口径是「投递数 + 丢弃/覆盖数 == 生产数」，
// 外加同一生产者内序号严格递增（单工作线程取出即队列顺序）。
auto smoke_concurrent_producers(ylog::OverflowPolicy policy, std::string_view label) -> void {
    constexpr auto producer_count = 4;
    constexpr auto per_producer = 50;
    constexpr auto total = producer_count * per_producer;

    auto state = std::make_shared<MeasuredSink::State>();
    auto reports = OverflowReports{};

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{
        .name = std::string(label),
        .queue_capacity = 16,
        .overflow_policy = policy,
        .err_handler = reports.handler(),
    });
    logger.append_sink(MeasuredSink{state}, "%v");

    auto producers = std::vector<std::jthread>{};
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&logger, producer] {
            for (int sequence = 0; sequence < per_producer; ++sequence) {
                logger.info({}, "{}-{}", producer, sequence);
            }
        });
    }
    // Block 策略下若唤醒丢失，这里会挂住而不是失败；ctest 的 TIMEOUT 是兜底。
    for (auto& producer : producers) {
        producer.join();
    }

    logger.flush();  // 屏障：返回时已入队的行全部落盘

    auto lines = snapshot(*state);
    auto dropped = int{0};
    if (policy == ylog::OverflowPolicy::OverrunOldest) {
        dropped = reports.overrun->load(std::memory_order_relaxed);
    } else if (policy == ylog::OverflowPolicy::DiscardNew) {
        dropped = reports.discarded->load(std::memory_order_relaxed);
    }

    check(lines.size() + static_cast<std::size_t>(dropped) == static_cast<std::size_t>(total),
          std::format("{}: 投递数 + 丢弃数 == 生产数", label));
    check(reports.other->load(std::memory_order_relaxed) == 0,
          std::format("{}: 只有计划内的溢出上报", label));
    check(reports.overrun->load(std::memory_order_relaxed) == 0 ||
              policy == ylog::OverflowPolicy::OverrunOldest,
          std::format("{}: 未误报覆盖", label));
    check(reports.discarded->load(std::memory_order_relaxed) == 0 ||
              policy == ylog::OverflowPolicy::DiscardNew,
          std::format("{}: 未误报丢弃", label));
    if (policy == ylog::OverflowPolicy::Block) {
        check(lines.size() == static_cast<std::size_t>(total), "Block: 一条不丢");
    } else {
        check(dropped > 0, std::format("{}: 小容量确实制造了丢弃", label));
    }

    auto last_sequence = std::array<int, producer_count>{};
    last_sequence.fill(-1);
    auto ordered = true;
    for (const auto& [producer, sequence] : lines) {
        if (producer < 0 || producer >= producer_count ||
            sequence <= last_sequence[static_cast<std::size_t>(producer)]) {
            ordered = false;
            break;
        }
        last_sequence[static_cast<std::size_t>(producer)] = sequence;
    }
    check(ordered, std::format("{}: 同一生产者内按 FIFO 落盘", label));
}

// 无异常构建下 sink 不抛，写失败只能经 sink 自己的 err_handler 上报；
// 关键在于工作线程不被这种失败杀掉，flush 仍能作为屏障返回。
auto async_survives_sink_write_failure() -> void {
    if (!std::filesystem::exists("/dev/full")) {
        std::println("skip: /dev/full unavailable");
        return;
    }

    auto state = std::make_shared<MeasuredSink::State>();
    auto sink_reports = std::make_shared<std::atomic<int>>(0);
    auto logger_reports = OverflowReports{};

    auto sink_config = ylog::RotatingFileConfig{};
    sink_config.filename = "/dev/full";
    sink_config.max_size = std::size_t{1} << 20;  // 够大，避免触发 rotate 分支
    sink_config.err_handler = [sink_reports](std::string_view) {
        sink_reports->fetch_add(1, std::memory_order_relaxed);
    };

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{
        .name = "async-sink-failure",
        .err_handler = logger_reports.handler(),
    });
    logger.append_sink(ylog::RotatingFileSink(sink_config), "%v");
    logger.append_sink(MeasuredSink{state}, "%v");

    logger.info({}, "0-0");
    logger.info({}, "0-1");
    logger.flush();

    check(sink_reports->load(std::memory_order_relaxed) > 0,
          "sink 写失败经注入的 err_handler 上报");
    check(snapshot(*state).size() == 2, "另一个 sink 不受失败影响");
    check(logger_reports.other->load(std::memory_order_relaxed) == 0,
          "sink 失败不经 AsyncLogger 的 err_handler 转手");
}

}  // namespace

auto main() -> int {
    auto logger = ylog::Logger("noexc");
    logger.set_level(ylog::Level::Info);

    auto out = std::string{};
    logger.append_sink(Capture{&out}, "%v");
    logger.info({}, "hello {}", 42);
    check(out == "hello 42\n", "info message reaches sink");

    out.clear();
    logger.debug({}, "filtered");
    check(out.empty(), "below-level message is filtered");

    // 配置成功路径：无异常构建下正常构造并写出。
    auto directory = temp_directory();
    auto path = directory / "app.log";
    {
        auto sink = ylog::FileSink(path.string());
        sink.write("file\n");
        sink.flush();
    }
    check(read_all(path) == "file\n", "file sink writes");

    auto rotating_config = ylog::RotatingFileConfig{};
    rotating_config.filename = (directory / "rotating.log").string();
    rotating_config.max_size = 16;
    {
        auto sink = ylog::RotatingFileSink(rotating_config);
        sink.write("rotating\n");
        sink.flush();
    }
    check(read_all(directory / "rotating.log") == "rotating\n", "rotating sink writes");
    std::filesystem::remove_all(directory);

    auto& registry = ylog::Registry::instance();
    auto& named = registry.create("noexc-app");
    named.set_level(ylog::Level::Debug);
    check(registry.get("noexc-app") == &named, "registry stores logger");

    // 异步组件在同一命名空间下注册，两个查找入口互不混淆。
    auto& async_named = registry.create_async("noexc-async", ylog::AsyncLoggerConfig{});
    check(registry.get_async("noexc-async") == &async_named, "registry stores async logger");
    check(registry.get("noexc-async") == nullptr, "async name is not a sync logger");

    check(registry.drop("noexc-app"), "registry drops logger");
    check(registry.drop("noexc-async"), "registry drops async logger");

    // 异步基础路径：构造 → 记录 → flush，flush 返回时行必须已落盘。
    auto async_state = std::make_shared<MeasuredSink::State>();
    {
        auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "noexc-async-direct"});
        logger.append_sink(MeasuredSink{async_state}, "%v");
        logger.info({}, "0-0");
        logger.info({}, "0-1");
        logger.flush();
        check(snapshot(*async_state).size() == 2, "flush 返回时行已落盘");
    }
    check(snapshot(*async_state).size() == 2, "析构排空不重复投递");

    smoke_concurrent_producers(ylog::OverflowPolicy::Block, "block");
    smoke_concurrent_producers(ylog::OverflowPolicy::OverrunOldest, "overrun_oldest");
    smoke_concurrent_producers(ylog::OverflowPolicy::DiscardNew, "discard_new");
    async_survives_sink_write_failure();

    reports_hot_path_failure();

    if (failures != 0) {
        std::println(std::cerr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("noexc checks passed");
    return 0;
}
