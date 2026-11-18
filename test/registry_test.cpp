#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(RegistryTest, CreateAndGet) {
    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create("test-app");
    logger.set_level(ylog::Level::Debug);

    EXPECT_EQ(registry.get("test-app")->name(), "test-app");
    EXPECT_EQ(registry.get("missing"), nullptr);
    EXPECT_TRUE(registry.contains("test-app"));

    auto& same = registry.create("test-app");
    EXPECT_EQ(&same, &logger);

    EXPECT_TRUE(registry.drop("test-app"));
    EXPECT_FALSE(registry.contains("test-app"));
}

TEST(RegistryTest, DefaultLoggerIsRegistrySlot) {
    auto& registry = ylog::Registry::instance();
    auto& logger = ylog::default_logger();

    // 默认 logger 与 create("default") 是同一个实例，没有第二份同名实现（ADR-0010）。
    EXPECT_EQ(&logger, &registry.create("default"));
    EXPECT_EQ(&logger, registry.get("default"));
    EXPECT_EQ(logger.name(), ylog::default_logger_name);
}

TEST(RegistryTest, DefaultSlotOwnsItsSinkSet) {
    auto& registry = ylog::Registry::instance();

    // 初始集合由 Registry 在创建期给定：恰好一个 StdoutSink（ADR-0011）。
    EXPECT_EQ(ylog::default_logger().sinks().size(), 1);

    // 保留名的集合不接受外部指定：给了 sink 就报错，而不是静默丢弃。
    EXPECT_THROW(registry.create(ylog::LoggerConfig{
                     .name = std::string(ylog::default_logger_name),
                     .sinks = ylog::make_sinks(ylog::AnySink{ylog::NullSink{}, "%v"})}),
                 std::invalid_argument);
}

TEST(RegistryTest, DefaultLoggerIsPermanent) {
    auto& registry = ylog::Registry::instance();
    auto* logger = &ylog::default_logger();

    EXPECT_FALSE(registry.drop("default"));
    EXPECT_TRUE(registry.contains("default"));
    EXPECT_EQ(&ylog::default_logger(), logger);

    registry.create("doomed");
    registry.drop_all();
    EXPECT_FALSE(registry.contains("doomed"));
    EXPECT_TRUE(registry.contains("default"));
    EXPECT_EQ(&ylog::default_logger(), logger);
}

TEST(RegistryTest, FreeFunctionsTargetDefaultSlot) {
    auto& registry = ylog::Registry::instance();

    ylog::set_level(ylog::Level::Warning);
    EXPECT_EQ(ylog::default_logger().level(), ylog::Level::Warning);

    // 按名字配置与自由函数作用于同一实例，两条路径可互换。
    EXPECT_TRUE(registry.set_level(ylog::default_logger_name, ylog::Level::Error));
    EXPECT_EQ(ylog::default_logger().level(), ylog::Level::Error);
}

TEST(RegistryAsyncTest, CreatesAndFindsAsyncLogger) {
    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create_async("async-registry", ylog::AsyncLoggerConfig{});

    EXPECT_EQ(logger.name(), "async-registry");
    EXPECT_EQ(registry.get_async("async-registry"), &logger);
    EXPECT_EQ(registry.get("async-registry"), nullptr);
    EXPECT_EQ(registry.get_async("missing"), nullptr);
    EXPECT_TRUE(registry.contains("async-registry"));

    registry.drop_all();
    EXPECT_FALSE(registry.contains("async-registry"));
}

TEST(RegistryAsyncTest, RejectsNameCollisionAcrossTypes) {
    auto& registry = ylog::Registry::instance();
    registry.create("collision-sync");

    EXPECT_THROW(registry.create_async("collision-sync", ylog::AsyncLoggerConfig{}),
                 std::invalid_argument);

    registry.create_async("collision-async", ylog::AsyncLoggerConfig{});
    EXPECT_THROW(registry.create("collision-async"), std::invalid_argument);

    registry.drop_all();
}

TEST(RegistryAsyncTest, SetLevelByNameFiltersAsyncMessages) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create_async(
        "async-level",
        ylog::AsyncLoggerConfig{.sinks = ylog::make_sinks(ylog::AnySink{view, "%v"})});

    EXPECT_TRUE(registry.set_level("async-level", ylog::Level::Warning));
    EXPECT_FALSE(registry.set_level("missing", ylog::Level::Warning));
    EXPECT_EQ(logger.level(), ylog::Level::Warning);

    logger.info({}, "dropped");
    logger.warning({}, "kept");

    ASSERT_TRUE(view.wait_for(1, std::chrono::milliseconds(1000)));

    auto lines = view.snapshot();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "kept\n");

    registry.drop_all();
}

TEST(RegistryAsyncTest, FlushAllIsBarrierForAsyncLoggers) {
    auto directory = temp_test_path("registry-async-flush");
    std::filesystem::create_directories(directory);
    auto path = directory / "flush.log";

    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create_async(
        "async-flush-all", ylog::AsyncLoggerConfig{.sinks = ylog::make_sinks(ylog::AnySink{
                                                       ylog::FileSink(path.string()), "%v"})});
    logger.info({}, "persisted");

    registry.flush_all();
    EXPECT_EQ(read_all(path), "persisted\n");

    registry.drop_all();
    std::filesystem::remove_all(directory);
}

TEST(RegistryAsyncTest, DropDrainsWithoutExplicitStop) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create_async(
        "async-drop",
        ylog::AsyncLoggerConfig{.sinks = ylog::make_sinks(ylog::AnySink{view, "%v"})});
    for (int i = 0; i < 100; ++i) {
        logger.info({}, "line-{}", i);
    }

    EXPECT_TRUE(registry.drop("async-drop"));
    EXPECT_FALSE(registry.contains("async-drop"));

    EXPECT_EQ(view.snapshot().size(), 100u) << "drop() 时没有排空已入队的消息";
}
