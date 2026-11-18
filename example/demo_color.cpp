module demo;

import std;
import ylog;

// 颜色是 sink 的配置而不是 sink 的种类：sink 只用 color_mode() 声明意愿，
// 上色由 AnySink 按 formatter 给出的 %^/%$ 区间执行。
void demo_color() {
    std::println("\n== 颜色 ==");

    // 两个 sink：第一个按 Automatic 在配置期定型（stdout 不是终端时定为 Never），
    // 第二个强制上色，所以同一行会以两种形态各输出一次。
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "color",
        .level = ylog::Level::Debug,
        .sinks = ylog::make_sinks(
            ylog::AnySink{ylog::StdoutSink{}},
            ylog::AnySink{ylog::StdoutSink{ylog::StdoutConfig{.color = ylog::ColorMode::Always}},
                          "%^[%l]%$ %v"}),
    });
    logger.info({}, "只有 %^ 与 %$ 之间会被着色");
    logger.error({}, "重定向到文件时，第二份仍带转义序列");
}
