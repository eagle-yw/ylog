#include "gtest/gtest.h"

import std;
import ylog;

struct Point {
    int x;
    double y;
};

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
    auto* original = std::cout.rdbuf();
    std::cout.rdbuf(oss.rdbuf());

    auto logger = ylog::Logger("struct-test");
    logger.set_level(ylog::Level::Info);
    auto point = Point{3, 4.0};
    logger.info({}, "{}", ylog::reflected(point));

    std::cout.rdbuf(original);
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
    // 验证位置包含测试文件名和行号
    EXPECT_TRUE(output.contains("unit_test.cpp"));
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

TEST(LoggerTest, AppendSink) {
    auto logger = ylog::Logger("test");
    logger.set_level(ylog::Level::Debug);

    struct TestSink {
        std::string* output;
        auto write(std::string_view msg) -> void { *output += msg; }
    };

    std::string output;
    logger.append_sink(TestSink{&output});
    logger.debug({}, "append sink test {}", 42);

    EXPECT_TRUE(output.ends_with("append sink test 42\n"));
}

TEST(FormatterTest, PatternFlagConcept) {
    struct ValidFlag {
        auto format(const ylog::Message&, const std::tm&, std::string&) -> void {}
    };
    struct ConstOnlyFlag {
        auto format(const ylog::Message&, const std::tm&, std::string&) const -> void {}
    };
    struct MissingFormat {};
    struct WrongArgument {
        auto format(const ylog::Message&, const std::tm&, int) -> void {}
    };
    struct WrongReturn {
        auto format(const ylog::Message&, const std::tm&, std::string&) -> int { return 0; }
    };
    struct NonCopyableFlag {
        NonCopyableFlag() = default;
        NonCopyableFlag(const NonCopyableFlag&) = delete;
        auto operator=(const NonCopyableFlag&) -> NonCopyableFlag& = delete;
        auto format(const ylog::Message&, const std::tm&, std::string&) -> void {}
    };

    static_assert(ylog::PatternFlag<ValidFlag>);
    static_assert(!ylog::PatternFlag<ConstOnlyFlag>);
    static_assert(!ylog::PatternFlag<MissingFormat>);
    static_assert(!ylog::PatternFlag<WrongArgument>);
    static_assert(!ylog::PatternFlag<WrongReturn>);
    static_assert(!ylog::PatternFlag<NonCopyableFlag>);
}

TEST(FormatterTest, CustomFlagRegistration) {
    struct Marker {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("custom");
        }
    };

    auto formatter = ylog::PatternFormatter("%x:%v");
    formatter.add_flag('x', Marker{});

    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    EXPECT_EQ(out, "custom:msg\n");
}

TEST(FormatterTest, CustomFlagOverridesBuiltin) {
    struct Override {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("OVERRIDE");
        }
    };

    auto formatter = ylog::PatternFormatter("%v");
    formatter.add_flag('v', Override{});

    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    EXPECT_EQ(out, "OVERRIDE\n");
}

TEST(FormatterTest, DuplicateCustomFlagThrows) {
    struct Marker {
        auto format(const ylog::Message&, const std::tm&, std::string&) -> void {}
    };

    auto formatter = ylog::PatternFormatter("%x");
    formatter.add_flag('x', Marker{});

    try {
        formatter.add_flag('x', Marker{});
        FAIL() << "duplicate registration should throw";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string_view(error.what()).find('x'), std::string_view::npos);
    }
}

TEST(FormatterTest, AddFlagRecompilesAndSuppliesTime) {
    struct TimeFlag {
        bool* saw_time;
        auto format(const ylog::Message&, const std::tm& time, std::string&) -> void {
            *saw_time = time.tm_year + 1900 >= 2000;
        }
    };

    auto formatter = ylog::PatternFormatter("%x");
    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    EXPECT_EQ(out, "%x\n");

    bool saw_time = false;
    formatter.add_flag('x', TimeFlag{&saw_time});
    out.clear();
    formatter.format(message, out);
    EXPECT_EQ(out, "\n");
    EXPECT_TRUE(saw_time);
}

TEST(FormatterTest, ClonePreservesCustomFlags) {
    struct Marker {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("CUSTOM");
        }
    };

    auto original = ylog::PatternFormatter("%x");
    original.add_flag('x', Marker{});

    auto copied = original.clone();
    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    copied->format(message, out);
    EXPECT_EQ(out, "CUSTOM\n");
}

TEST(FormatterTest, CloneRegistrationsAreIndependent) {
    struct Marker {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("A");
        }
    };
    struct Extra {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("B");
        }
    };

    auto original = ylog::PatternFormatter("%x");
    original.add_flag('x', Marker{});
    auto copied = original.clone();
    auto& copied_pattern = dynamic_cast<ylog::PatternFormatter&>(*copied);
    copied_pattern.add_flag('q', Extra{});
    copied_pattern.set_pattern("%x%q");

    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string copied_out, original_out;
    copied_pattern.format(message, copied_out);
    EXPECT_EQ(copied_out, "AB\n");

    original.set_pattern("%x%q");
    original.format(message, original_out);
    EXPECT_EQ(original_out, "A%q\n");
}

TEST(FormatterTest, CustomFlagPadding) {
    struct Marker {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("ab");
        }
    };

    auto formatter = ylog::PatternFormatter("%-5x|%v");
    formatter.add_flag('x', Marker{});

    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    EXPECT_EQ(out, "ab   |msg\n");
}

TEST(FormatterTest, CustomFlagOccurrencesAreIndependent) {
    struct Counter {
        int count{0};
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append(std::to_string(++count));
        }
    };

    auto formatter = ylog::PatternFormatter("%x%x");
    formatter.add_flag('x', Counter{});

    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    EXPECT_EQ(out, "11\n");
}

TEST(FormatterTest, CustomFlagReceivesMessage) {
    struct NameFlag {
        std::string* name;
        auto format(const ylog::Message& message, const std::tm&, std::string&) -> void {
            *name = std::string(message.logger_name());
        }
    };

    auto formatter = ylog::PatternFormatter("%x");
    std::string seen;
    formatter.add_flag('x', NameFlag{&seen});

    auto message = ylog::Message(ylog::Level::Info, "logger-name", {}, "msg");
    std::string out;
    formatter.format(message, out);
    EXPECT_EQ(seen, "logger-name");
    EXPECT_EQ(out, "\n");
}

TEST(FormatterTest, UnknownFlagRemainsLiteral) {
    auto formatter = ylog::PatternFormatter("%q");
    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    EXPECT_EQ(out, "%q\n");
}

TEST(FormatterTest, PatternFlags) {
    struct Capture { std::string* out; auto write(std::string_view s) -> void { *out += s; } };

    auto logger = ylog::Logger("flags");
    logger.set_level(ylog::Level::Info);
    std::string out;
    // 只用已知 flag: %L(短级别) %n(名称) %v(payload) %%(字面百分号)
    logger.append_sink(Capture{&out}, "[%L] %n: %v 100%%");
    logger.info({}, "hello");

    EXPECT_EQ(out, "[I] flags: hello 100%\n");
}

TEST(FormatterTest, PerSinkPattern) {
    struct Capture { std::string* out; auto write(std::string_view s) -> void { *out += s; } };

    auto logger = ylog::Logger("multi");
    logger.set_level(ylog::Level::Info);
    std::string short_out, long_out;
    // 两个 sink 使用不同 pattern —— 移植自 spdlog 的核心设计
    logger.append_sink(Capture{&short_out}, "%v");
    logger.append_sink(Capture{&long_out}, "[%L] %v");
    logger.info({}, "same message");

    EXPECT_EQ(short_out, "same message\n");
    EXPECT_EQ(long_out, "[I] same message\n");
}

TEST(FormatterTest, SetPatternFanout) {
    struct Capture { std::string* out; auto write(std::string_view s) -> void { *out += s; } };

    auto logger = ylog::Logger("fanout");
    logger.set_level(ylog::Level::Info);
    std::string a, b;
    logger.append_sink(Capture{&a}, "%v");
    logger.append_sink(Capture{&b}, "%v");
    logger.set_pattern("<%L:%v>");
    logger.warning({}, "changed");

    EXPECT_EQ(a, "<W:changed>\n");
    EXPECT_EQ(b, "<W:changed>\n");
}

TEST(FormatterTest, PerSinkCustomFlagRegistrations) {
    struct Capture { std::string* out; auto write(std::string_view s) -> void { *out += s; } };
    struct First {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("FIRST");
        }
    };
    struct Second {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("SECOND");
        }
    };

    auto first = std::make_unique<ylog::PatternFormatter>("%x");
    first->add_flag('x', First{});
    auto second = std::make_unique<ylog::PatternFormatter>("%x");
    second->add_flag('x', Second{});

    auto logger = ylog::Logger("custom-sinks");
    logger.set_level(ylog::Level::Info);
    std::string a, b;
    logger.append_sink(Capture{&a}, std::move(first));
    logger.append_sink(Capture{&b}, std::move(second));
    logger.info({}, "msg");

    EXPECT_EQ(a, "FIRST\n");
    EXPECT_EQ(b, "SECOND\n");
}

TEST(FormatterTest, SetPatternFanoutPreservesCustomFlags) {
    struct Capture { std::string* out; auto write(std::string_view s) -> void { *out += s; } };
    struct Marker {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("C");
        }
    };

    auto first = std::make_unique<ylog::PatternFormatter>("%x");
    first->add_flag('x', Marker{});
    auto second = std::make_unique<ylog::PatternFormatter>("%x");
    second->add_flag('x', Marker{});

    auto logger = ylog::Logger("fanout-custom");
    logger.set_level(ylog::Level::Info);
    std::string a, b;
    logger.append_sink(Capture{&a}, std::move(first));
    logger.append_sink(Capture{&b}, std::move(second));
    logger.set_pattern("[%x]");
    logger.info({}, "msg");

    EXPECT_EQ(a, "[C]\n");
    EXPECT_EQ(b, "[C]\n");
}

TEST(LoggerTest, SetPatternRejectsNonPatternFormatter) {
    struct Capture { std::string* out; auto write(std::string_view s) -> void { *out += s; } };
    struct PlainFormatter final : ylog::Formatter {
        auto format(const ylog::Message&, std::string& dest) -> void override {
            dest.append("plain\n");
        }
        auto clone() const -> std::unique_ptr<ylog::Formatter> override {
            return std::make_unique<PlainFormatter>();
        }
        auto pattern() const -> std::string_view override { return ""; }
    };

    auto logger = ylog::Logger("plain-formatter");
    logger.set_level(ylog::Level::Info);
    std::string out;
    logger.append_sink(Capture{&out}, std::make_unique<PlainFormatter>());

    EXPECT_THROW(logger.set_pattern("%v"), std::invalid_argument);
    logger.info({}, "msg");
    EXPECT_EQ(out, "plain\n");
}

TEST(FormatterTest, Padding) {
    auto formatter = ylog::PatternFormatter("%-8l|%v");
    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    // "Info" 左对齐补到 8 宽度 -> "Info    |msg\n"
    EXPECT_EQ(out, "Info    |msg\n");
}

TEST(LoggerTest, FileSink) {
    auto sink = ylog::FileSink("test.log");
    auto logger = ylog::Logger("file-test");
    logger.set_level(ylog::Level::Debug);
    logger.append_sink(std::move(sink));

    logger.debug({}, "test sink file");
    logger.info({}, "test sink file");
    logger.error({}, "test sink file");
}

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

TEST(SinkTest, ConceptCheck) {
    static_assert(ylog::Sink<ylog::StdoutSink>);
    static_assert(ylog::Sink<ylog::FileSink>);

    struct GoodSink {
        auto write(std::string_view) -> void {}
    };
    struct BadSink {
        auto write(int) -> void {}
    };
    static_assert(ylog::Sink<GoodSink>);
    static_assert(!ylog::Sink<BadSink>);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
