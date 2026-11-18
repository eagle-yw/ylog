module demo;

import std;
import ylog;

// 运行期可改的三样：级别、flush 阈值、sink 视图。
void demo_runtime() {
    std::println("\n== 运行期控制 ==");

    auto logger = ylog::Logger("runtime");
    logger.set_level(ylog::Level::Info);

    // should_log 是调用方的守门函数：昂贵的参数构造与 std::format 都可以先跳过。
    if (logger.should_log(ylog::Level::Debug)) {
        logger.debug({}, "这条不会进来");
    }
    std::println("should_log(Debug) = {}", logger.should_log(ylog::Level::Debug));

    // flush_on 设阈值：达到该级别的消息写出后主动落盘，默认 Off 从不触发。
    logger.flush_on(ylog::Level::Error);
    logger.warning({}, "warning 不触发 flush");
    logger.error({}, "error 触发 flush");
    std::println("flush 阈值 = {}", ylog::level_name(logger.flush_level()));

    // sinks() 给出结构不可变的视图：元素可各自配置，增删仍要经 append_sink。
    std::println("sink 数 = {}", logger.sinks().size());
}
