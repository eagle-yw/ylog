module demo;

import std;
import ylog;

// 颜色是 sink 的配置而不是 sink 的种类：sink 只用 color_mode() 声明意愿，
// 上色由 AnySink 按 formatter 给出的 %^/%$ 区间执行。
void demo_color() {
    std::println("\n== 颜色 ==");

    auto logger = ylog::Logger("color");
    logger.set_level(ylog::Level::Debug);

    // 构造时那个 sink 按 Automatic 在配置期定型（stdout 不是终端时定为 Never）；
    // 这里追加的强制上色，所以同一行会以两种形态各输出一次。
    logger.append_sink(ylog::StdoutSink{ylog::StdoutConfig{.color = ylog::ColorMode::Always}},
                       "%^[%l]%$ %v");
    logger.info({}, "只有 %^ 与 %$ 之间会被着色");
    logger.error({}, "重定向到文件时，第二份仍带转义序列");
}
