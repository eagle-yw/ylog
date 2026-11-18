import std;
import ylog;

struct Point {
    int x = 1;
    double y = 1.0;
};

int main() {
    auto logger = ylog::Logger("demo");
    logger.set_level(ylog::Level::Trace);

    // 第一个参数 {} 触发 SourceLocation 默认构造，在调用点捕获位置
    logger.debug({}, "level is {}, {}", "debug", "009");
    logger.info({}, "level is {}, {}", "info", "010");
    logger.warning({}, "level is {}, {}", "warning", "010");
    logger.error({}, "level is {}, {}", "error", "010");
    logger.critical({}, "level is {}, {}", "critical", "010");

    ylog::set_level(ylog::Level::Debug);
    ylog::debug({}, "free function, value = {}", 42);

    // 结构化日志：reflected() 反射自动展开 Point 的所有字段
    auto point = Point{10, 20.1};
    logger.info({}, "{}", ylog::reflected(point));             // 直接打印
    logger.info({}, "point = {}", ylog::reflected(point));     // 嵌入任意格式串
    ylog::debug({}, "point = {}", ylog::reflected(point));      

    // 简化：reflected() 让任意结构体直接用于 std::format / println
    // 无需手写 constexpr 格式串和 splice
    std::println("reflect: {}", ylog::reflected(point));
    std::println("with label: point={}", ylog::reflected(point));

    // 反射驱动的级别名称（编译期）
    static_assert(ylog::level_name(ylog::Level::Warning) == "Warning");

    // 迁移自 spdlog：formatter 归属 sink，每个 sink 可有不同 pattern。
    auto multi = ylog::Logger("multi");
    multi.set_level(ylog::Level::Debug);
    // 默认 stdout sink：完整格式
    // 追加第二个 stdout sink：只输出级别和消息
    multi.append_sink(ylog::StdoutSink{}, "[%L] %v");
    multi.info({}, "同一条日志，多种格式");

    // set_pattern 扇出到所有 sink
    multi.set_pattern("%l | %v");
    multi.warning({}, "扇出后的统一格式");

    return 0;
}
