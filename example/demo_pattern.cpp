module demo;

import std;
import ylog;

// formatter 归属 sink：同一个 logger 的多个 sink 可以各用一套 pattern。
void demo_pattern() {
    std::println("\n== 每 sink 一套 pattern ==");

    auto logger = ylog::Logger("pattern");
    logger.set_level(ylog::Level::Debug);

    // 构造时已带一个 stdout sink（默认 pattern）；再追加一个只打级别与消息的。
    logger.append_sink(ylog::StdoutSink{}, "[%L] %v");
    logger.info({}, "同一条日志，两种格式");

    // set_pattern 扇出到所有 sink，覆盖它们各自的 pattern：两个 sink 各输出一行，
    // 内容相同正说明扇出生效。
    logger.set_pattern("%l | %v");
    logger.warning({}, "扇出后的统一格式");
}
