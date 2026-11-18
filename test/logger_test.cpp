#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(LoggerTest, LevelNamesFromTable) {
    static_assert(ylog::level_name(ylog::Level::Trace) == "Trace");
    static_assert(ylog::level_name(ylog::Level::Debug) == "Debug");
    static_assert(ylog::level_name(ylog::Level::Info) == "Info");
    static_assert(ylog::level_name(ylog::Level::Warning) == "Warning");
    static_assert(ylog::level_name(ylog::Level::Error) == "Error");
    static_assert(ylog::level_name(ylog::Level::Critical) == "Critical");
    static_assert(ylog::level_name(ylog::Level::Off) == "Off");

    EXPECT_EQ(ylog::level_from_string("Info"), ylog::Level::Info);
    EXPECT_EQ(ylog::level_from_string("unknown"), ylog::Level::Info);
}

TEST(LoggerTest, FieldFormatReturnsStringView) {
    static_assert(std::same_as<decltype(ylog::field_format<Point>()), std::string_view>);
    EXPECT_EQ(ylog::field_format<Point>(), "x={} y={}");
}

TEST(LoggerTest, ReflectedFromReflection) {
    auto point = Point{10, 20.5};
    // 反射自动展开字段并用编译期格式串格式化
    EXPECT_EQ(std::format("{}", ylog::reflected(point)), "x=10 y=20.5");

    // to_tuple 可单独使用
    auto fields = ylog::to_tuple(point);
    EXPECT_EQ(std::get<0>(fields), 10);
    EXPECT_EQ(std::get<1>(fields), 20.5);
}

TEST(LoggerTest, ReflectedInLogger) {
    auto oss = std::ostringstream();
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "struct-test",
        .sinks =
            ylog::make_sinks(ylog::AnySink{ylog::OstreamSink{ylog::OstreamConfig{.stream = &oss}}}),
    });
    logger.set_level(ylog::Level::Info);
    auto point = Point{3, 4.0};
    logger.info({}, "{}", ylog::reflected(point));

    EXPECT_TRUE(oss.str().contains("x=3 y=4"));
}

TEST(LoggerTest, SourceLocationNoMacro) {
    // {} 默认构造在调用点捕获位置（无需宏）
    auto oss = std::ostringstream();
    auto* original = std::cout.rdbuf();
    std::cout.rdbuf(oss.rdbuf());

    ylog::set_level(ylog::Level::Debug);
    int current_line = __LINE__ + 1;
    ylog::debug({}, "location test");

    std::cout.rdbuf(original);
    auto output = oss.str();
    // 验证位置包含本 TU 的文件名和行号（套件拆分后文件不再叫 unit_test.cpp）
    EXPECT_TRUE(output.contains("logger_test.cpp"));
    EXPECT_TRUE(output.contains(std::to_string(current_line)));
}

TEST(LoggerTest, DefaultLogger) {
    auto oss = std::ostringstream();
    auto* original_buffer = std::cout.rdbuf();
    std::cout.rdbuf(oss.rdbuf());

    ylog::set_level(ylog::Level::Debug);
    ylog::debug({}, "default logger level is {}", "debug");

    auto log_output = oss.str();
    EXPECT_TRUE(log_output.ends_with("default logger level is debug\n"));

    std::cout.rdbuf(original_buffer);
}

TEST(LoggerTest, ConfiguredSinkReceivesLines) {
    struct TestSink {
        std::string* output;
        auto write(std::string_view msg) -> void { *output += msg; }
    };

    auto output = std::string{};
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "test",
        .level = ylog::Level::Debug,
        .sinks = ylog::make_sinks(ylog::AnySink{TestSink{&output}}),
    });
    logger.debug({}, "configured sink test {}", 42);

    EXPECT_TRUE(output.ends_with("configured sink test 42\n"));
}

TEST(LoggerTest, ZeroSinksDiscardsMessages) {
    // 零 sink 是合法状态：没有目的地，flush 也不因此失败。
    auto logger = ylog::Logger(ylog::LoggerConfig{.name = "no-sinks"});
    EXPECT_TRUE(logger.sinks().empty());

    EXPECT_NO_THROW(logger.info({}, "dropped"));
    EXPECT_NO_THROW(logger.flush());
}

TEST(LoggerTest, ShouldLogFollowsLevel) {
    auto logger = ylog::Logger(ylog::LoggerConfig{.name = "should-log"});

    EXPECT_EQ(logger.level(), ylog::Level::Info);
    EXPECT_FALSE(logger.should_log(ylog::Level::Debug));
    EXPECT_TRUE(logger.should_log(ylog::Level::Info));
    EXPECT_TRUE(logger.should_log(ylog::Level::Critical));

    logger.set_level(ylog::Level::Debug);
    EXPECT_FALSE(logger.should_log(ylog::Level::Trace));
    EXPECT_TRUE(logger.should_log(ylog::Level::Debug));

    logger.set_level(ylog::Level::Off);
    EXPECT_FALSE(logger.should_log(ylog::Level::Critical));
}

TEST(LoggerTest, SinksAreFixedAtConstruction) {
    auto first = std::string{};
    auto second = std::string{};
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "sinks",
        .sinks = ylog::make_sinks(ylog::AnySink{CaptureSink{&first}, "%v"},
                                  ylog::AnySink{CaptureSink{&second}, "%v"}),
    });
    logger.set_level(ylog::Level::Info);

    EXPECT_EQ(logger.sinks().size(), 2);

    // 元素可配置：单个 sink 的 pattern 只影响自己。
    logger.sinks()[0].set_pattern("%l: %v");
    logger.sinks()[1].set_pattern("[%v]");

    logger.info({}, "hello");
    logger.flush();

    EXPECT_EQ(first, "Info: hello\n");
    EXPECT_EQ(second, "[hello]\n");
}

TEST(LoggerTest, SetFormatterFansOutToEverySink) {
    auto first = std::string{};
    auto second = std::string{};
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "set-formatter",
        .sinks = ylog::make_sinks(ylog::AnySink{CaptureSink{&first}},
                                  ylog::AnySink{CaptureSink{&second}}),
    });
    logger.set_level(ylog::Level::Info);

    logger.set_formatter(std::make_unique<ylog::PatternFormatter>("%l|%v"));
    logger.info({}, "hello");
    EXPECT_EQ(first, "Info|hello\n");
    EXPECT_EQ(second, "Info|hello\n");

    // formatter 归属 sink：扇出的是各自的 clone，改一个不影响另一个。
    logger.sinks()[0].set_pattern("%v");
    logger.info({}, "again");
    EXPECT_EQ(first, "Info|hello\nagain\n");
    EXPECT_EQ(second, "Info|hello\nInfo|again\n");
}

TEST(LoggerTest, SetFormatterRejectsEmptyFormatter) {
    auto logger = ylog::Logger(ylog::LoggerConfig{.name = "empty-formatter"});

    EXPECT_THROW(logger.set_formatter(nullptr), std::invalid_argument);
}

TEST(LoggerTest, FlushOnTriggersOnlyForConfiguredLevels) {
    auto flushes = std::atomic<int>{0};
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "flush-on",
        .level = ylog::Level::Trace,
        .sinks = ylog::make_sinks(ylog::AnySink{FlushCountingSink{&flushes}}),
    });

    // 默认 Off：日志路径一次都不 flush。
    EXPECT_EQ(logger.flush_level(), ylog::Level::Off);
    logger.info({}, "default is off");
    logger.critical({}, "default is off");
    EXPECT_EQ(flushes.load(), 0);

    logger.flush_on(ylog::Level::Warning);
    EXPECT_EQ(logger.flush_level(), ylog::Level::Warning);

    logger.info({}, "below the threshold");
    EXPECT_EQ(flushes.load(), 0);

    logger.warning({}, "at the threshold");
    EXPECT_EQ(flushes.load(), 1);

    logger.error({}, "above the threshold");
    EXPECT_EQ(flushes.load(), 2);
}

TEST(LoggerTest, SetPatternRejectsNonPatternFormatter) {
    struct PlainFormatter final : ylog::Formatter {
        auto format(const ylog::Message&, std::string& dest) -> void override {
            dest.append("plain\n");
        }
        auto clone() const -> std::unique_ptr<ylog::Formatter> override {
            return std::make_unique<PlainFormatter>();
        }
        auto pattern() const -> std::string_view override { return ""; }
    };

    auto out = std::string{};
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "plain-formatter",
        .sinks =
            ylog::make_sinks(ylog::AnySink{CaptureSink{&out}, std::make_unique<PlainFormatter>()}),
    });
    logger.set_level(ylog::Level::Info);

    EXPECT_THROW(logger.set_pattern("%v"), std::invalid_argument);
    logger.info({}, "msg");
    EXPECT_EQ(out, "plain\n");
}

TEST(LoggerTest, FileSink) {
    auto directory = temp_test_path("logger-file-sink");
    std::filesystem::create_directories(directory);
    auto path = directory / "sink.log";

    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "file-test",
        .level = ylog::Level::Debug,
        .sinks = ylog::make_sinks(ylog::AnySink{ylog::FileSink(path.string()), "%v"}),
    });

    logger.debug({}, "debug line");
    logger.info({}, "info line");
    logger.error({}, "error line");
    logger.flush();

    EXPECT_EQ(read_all(path), "debug line\ninfo line\nerror line\n");

    std::filesystem::remove_all(directory);
}
