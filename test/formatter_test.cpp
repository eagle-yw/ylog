#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

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

TEST(FormatterTest, ColorRangeIsReadableThroughTheBaseClass) {
    auto formatter = ylog::PatternFormatter("%^%l%$ %v");
    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    auto out = std::string{};
    formatter.format(message, out);

    // 只有基类有虚函数时，类型擦除层才拿得到区间。
    const ylog::Formatter& base = formatter;
    auto [start, end] = base.color_range();
    EXPECT_EQ(out.substr(start, end - start), "Info");
    EXPECT_EQ(out, "Info msg\n");
}

TEST(FormatterTest, DefaultColorRangeIsEmpty) {
    struct PlainFormatter final : ylog::Formatter {
        auto format(const ylog::Message&, std::string& dest) -> void override {
            dest.append("plain\n");
        }
        auto clone() const -> std::unique_ptr<ylog::Formatter> override {
            return std::make_unique<PlainFormatter>(*this);
        }
        auto pattern() const -> std::string_view override { return ""; }
    };

    auto formatter = PlainFormatter{};
    EXPECT_EQ(formatter.color_range(), (std::pair<std::size_t, std::size_t>{0, 0}));
}

TEST(FormatterTest, FullPatternMarksTheLevelNameForColor) {
    auto formatter = ylog::PatternFormatter("%+");
    auto message = ylog::Message(ylog::Level::Warning, "n", {}, "msg");
    auto out = std::string{};
    formatter.format(message, out);

    auto [start, end] = formatter.color_range();
    ASSERT_LT(start, end);
    EXPECT_EQ(out.substr(start, end - start), "Warning");
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

    auto formatter = ylog::PatternFormatter("%Q");
    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    EXPECT_EQ(out, "%Q\n");

    bool saw_time = false;
    formatter.add_flag('Q', TimeFlag{&saw_time});
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
    auto out = std::string{};
    // 只用已知 flag: %L(短级别) %n(名称) %v(payload) %%(字面百分号)
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "flags",
        .sinks = ylog::make_sinks(ylog::AnySink{CaptureSink{&out}, "[%L] %n: %v 100%%"}),
    });
    logger.info({}, "hello");

    EXPECT_EQ(out, "[I] flags: hello 100%\n");
}

TEST(FormatterTest, PerSinkPattern) {
    auto short_out = std::string{};
    auto long_out = std::string{};
    // 两个 sink 使用不同 pattern —— 移植自 spdlog 的核心设计
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "multi",
        .sinks = ylog::make_sinks(ylog::AnySink{CaptureSink{&short_out}, "%v"},
                                  ylog::AnySink{CaptureSink{&long_out}, "[%L] %v"}),
    });
    logger.info({}, "same message");

    EXPECT_EQ(short_out, "same message\n");
    EXPECT_EQ(long_out, "[I] same message\n");
}

TEST(FormatterTest, SetPatternFanout) {
    auto a = std::string{};
    auto b = std::string{};
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "fanout",
        .sinks = ylog::make_sinks(ylog::AnySink{CaptureSink{&a}, "%v"},
                                  ylog::AnySink{CaptureSink{&b}, "%v"}),
    });
    logger.set_pattern("<%L:%v>");
    logger.warning({}, "changed");

    EXPECT_EQ(a, "<W:changed>\n");
    EXPECT_EQ(b, "<W:changed>\n");
}

TEST(FormatterTest, PerSinkCustomFlagRegistrations) {
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

    auto a = std::string{};
    auto b = std::string{};
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "custom-sinks",
        .sinks = ylog::make_sinks(ylog::AnySink{CaptureSink{&a}, std::move(first)},
                                  ylog::AnySink{CaptureSink{&b}, std::move(second)}),
    });
    logger.info({}, "msg");

    EXPECT_EQ(a, "FIRST\n");
    EXPECT_EQ(b, "SECOND\n");
}

TEST(FormatterTest, SetPatternFanoutPreservesCustomFlags) {
    struct Marker {
        auto format(const ylog::Message&, const std::tm&, std::string& dest) -> void {
            dest.append("C");
        }
    };

    auto first = std::make_unique<ylog::PatternFormatter>("%x");
    first->add_flag('x', Marker{});
    auto second = std::make_unique<ylog::PatternFormatter>("%x");
    second->add_flag('x', Marker{});

    auto a = std::string{};
    auto b = std::string{};
    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "fanout-custom",
        .sinks = ylog::make_sinks(ylog::AnySink{CaptureSink{&a}, std::move(first)},
                                  ylog::AnySink{CaptureSink{&b}, std::move(second)}),
    });
    logger.set_pattern("[%x]");
    logger.info({}, "msg");

    EXPECT_EQ(a, "[C]\n");
    EXPECT_EQ(b, "[C]\n");
}

TEST(FormatterTest, Padding) {
    auto formatter = ylog::PatternFormatter("%-8l|%v");
    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");
    std::string out;
    formatter.format(message, out);
    // "Info" 左对齐补到 8 宽度 -> "Info    |msg\n"
    EXPECT_EQ(out, "Info    |msg\n");
}
