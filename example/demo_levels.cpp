module demo;

import std;
import ylog;

// 六个级别。第一个参数 {} 触发 SourceLocation 的默认构造，在调用点捕获位置，
// pattern 的 %@ 输出它。
void demo_levels() {
    std::println("\n== 级别 ==");

    auto logger = ylog::Logger("levels");
    logger.set_level(ylog::Level::Trace);

    logger.trace({}, "trace {}", 1);
    logger.debug({}, "debug {}", 2);
    logger.info({}, "info {}", 3);
    logger.warning({}, "warning {}", 4);
    logger.error({}, "error {}", 5);
    logger.critical({}, "critical {}", 6);

    // 自由函数作用于默认 logger：它是 Registry 中名为 default 的常驻槽位（ADR-0010），
    // 因此这与 Registry::set_level("default", …) 是同一件事。
    ylog::set_level(ylog::Level::Debug);
    ylog::debug({}, "free function, value = {}", 42);
    ylog::info({}, "free function, info");
}
