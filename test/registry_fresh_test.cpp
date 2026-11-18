#include "gtest/gtest.h"

import std;
import ylog;

// 独立进程里的 async-first 时序：default 保留名在槽位懒创建之前就已经生效。
// unit_test 是共享进程，里面的 default 槽位多半已经创建，覆盖不到「新建 Registry 上先用
// 异步注册占坑」这条缺陷路径，所以这组断言单独一个可执行文件承载。
TEST(RegistryFreshTest, ReservedNameHoldsBeforeLazyCreation) {
    auto& registry = ylog::Registry::instance();

    // 进程刚启动：槽位还没人用过，如实为 false。
    EXPECT_FALSE(registry.contains("default"));

    EXPECT_THROW(
        registry.create_async(std::string(ylog::default_logger_name), ylog::AsyncLoggerConfig{}),
        std::invalid_argument);

    // 失败的注册不留下任何槽位：既没有异步组件，也没有被预占的同步条目。
    EXPECT_FALSE(registry.contains("default"));
    EXPECT_EQ(registry.get_async("default"), nullptr);

    // 随后懒创建按 ADR-0011 定型：恰好一个 StdoutSink，且与 default_logger() 同一实例。
    EXPECT_EQ(&registry.create("default"), &ylog::default_logger());
    EXPECT_EQ(ylog::default_logger().sinks().size(), 1u);

    // 常驻：不可摘除。
    EXPECT_FALSE(registry.drop("default"));
    EXPECT_TRUE(registry.contains("default"));
}
