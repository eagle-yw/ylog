module demo;

import std;
import ylog;

// sink 集合在构造期一次给定；同一条日志因此同时到达所有目的地。
void demo_sinks() {
    std::println("\n== sink 家族 ==");

    auto buffer = std::ostringstream{};
    auto collected = std::vector<std::string>{};
    auto store = ylog::RingBufferStore{std::size_t{3}};

    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "sinks",
        .level = ylog::Level::Debug,
        .sinks = ylog::make_sinks(
            ylog::AnySink{ylog::StdoutSink{}},
            ylog::AnySink{ylog::OstreamSink{ylog::OstreamConfig{.stream = &buffer,
                                                                .color = ylog::ColorMode::Never}},
                          "[ostream] %v"},
            ylog::AnySink{
                ylog::CallbackSink{ylog::CallbackConfig{
                    .fn = [&collected](std::string_view line) { collected.emplace_back(line); }}},
                "[callback] %v"},
            // 读句柄由调用方持有，sink 只存指针：store 必须比 logger 活得久。
            ylog::AnySink{ylog::RingBufferSink{store}, "[ring] %v"},
            // 无状态、无配置、也不提供 flush：sink 协议的可选部分由它验证。
            ylog::AnySink{ylog::NullSink{}}),
    });

    for (auto i = 0; i < 5; ++i) {
        logger.info({}, "line {}", i);
    }

    const auto text = buffer.str();
    std::println("ostream 收到 {} 行", std::ranges::count(text, '\n'));
    std::println("callback 收到 {} 行", collected.size());
    std::println("ring buffer 容量 {}，现有 {} 条：", store.capacity(), store.size());
    for (const auto& line : store.last(store.capacity())) {
        std::print("  {}", line);  // 存的是整行，自带换行
    }
}
