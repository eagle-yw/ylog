module demo;

import std;
import ylog;

namespace {

struct Point {
    int x = 1;
    double y = 1.0;
};

}  // namespace

// reflected() 反射展开聚合类型的所有字段，产出的 FieldFormat 可直接用于格式串。
void demo_reflect() {
    std::println("\n== 结构化日志 ==");

    auto logger = ylog::Logger("reflect");
    const auto point = Point{10, 20.1};

    logger.info({}, "{}", ylog::reflected(point));
    logger.info({}, "point = {}", ylog::reflected(point));

    // 同一份 FieldFormat 也直接可用于 std::format / println，不必经过 logger。
    std::println("reflect: {}", ylog::reflected(point));

    // 级别名称由反射在编译期生成。
    static_assert(ylog::level_name(ylog::Level::Warning) == "Warning");
}
