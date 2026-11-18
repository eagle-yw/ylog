module demo;

import std;
import ylog;

// 每个 Logger 构造时自带一个 stdout sink；下面追加的 sink 与它并列，
// 同一条日志因此同时到达所有目的地。
void demo_sinks() {
    std::println("\n== sink 家族 ==");

    auto buffer = std::ostringstream{};
    auto collected = std::vector<std::string>{};
    auto store = ylog::RingBufferStore{std::size_t{3}};

    auto logger = ylog::Logger("sinks");
    logger.set_level(ylog::Level::Debug);

    logger.append_sink(
        ylog::OstreamSink{ylog::OstreamConfig{.stream = &buffer, .color = ylog::ColorMode::Never}},
        "[ostream] %v");

    logger.append_sink(
        ylog::CallbackSink{ylog::CallbackConfig{
            .fn = [&collected](std::string_view line) { collected.emplace_back(line); }}},
        "[callback] %v");

    // 读句柄由调用方持有，sink 只存指针：store 必须比 logger 活得久。
    logger.append_sink(ylog::RingBufferSink{store}, "[ring] %v");

    // 无状态、无配置、也不提供 flush：sink 协议的可选部分由它验证。
    logger.append_sink(ylog::NullSink{});

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
