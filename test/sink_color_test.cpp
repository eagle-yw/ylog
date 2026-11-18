#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(AnySinkColorTest, ColorsTheMarkedRangeWhenTheSinkAcceptsColor) {
    EXPECT_EQ(ostream_sink_output(ylog::Level::Info, ylog::ColorMode::Always, "%^%l%$ %v"),
              "\x1b[32mInfo\x1b[0m msg\n");
}

TEST(AnySinkColorTest, PrefixFollowsTheMessageLevel) {
    struct Case {
        ylog::Level level;
        std::string_view prefix;
    };
    constexpr auto cases = std::array{
        Case{ylog::Level::Trace, "\x1b[90m"}, Case{ylog::Level::Debug, "\x1b[36m"},
        Case{ylog::Level::Info, "\x1b[32m"},  Case{ylog::Level::Warning, "\x1b[33m"},
        Case{ylog::Level::Error, "\x1b[31m"}, Case{ylog::Level::Critical, "\x1b[1;31m"},
    };

    for (const auto& [level, prefix] : cases) {
        EXPECT_EQ(ostream_sink_output(level, ylog::ColorMode::Always, "%^%v%$"),
                  std::format("{}{}\x1b[0m\n", prefix, "msg"))
            << "level " << ylog::level_name(level);
    }
}

TEST(AnySinkColorTest, NeverLeavesTheLineByteIdentical) {
    EXPECT_EQ(ostream_sink_output(ylog::Level::Info, ylog::ColorMode::Never, "%^%l%$ %v"),
              "Info msg\n");
}

TEST(AnySinkColorTest, NoRangeMeansNoEscapeBytes) {
    EXPECT_EQ(ostream_sink_output(ylog::Level::Critical, ylog::ColorMode::Always, "%l %v"),
              "Critical msg\n");
}

TEST(AnySinkColorTest, FullPatternColorsOnlyTheLevelName) {
    auto output = ostream_sink_output(ylog::Level::Warning, ylog::ColorMode::Always, "%+");
    EXPECT_TRUE(output.contains("\x1b[33mWarning\x1b[0m")) << output;
}

TEST(AnySinkColorTest, AutomaticIsTreatedAsAcceptingColor) {
    // 协议口径是「非 Never 即接受」；把 Automatic 定型成两态是具体 sink 的责任。
    struct AutomaticSink {
        std::string* out;
        auto write(std::string_view line) -> void { out->append(line); }
        auto color_mode() const -> ylog::ColorMode { return ylog::ColorMode::Automatic; }
    };

    auto out = std::string{};
    auto any =
        ylog::AnySink(AutomaticSink{&out}, std::make_unique<ylog::PatternFormatter>("%^%l%$"));
    any.write(ylog::Message(ylog::Level::Error, "n", {}, "msg"));

    EXPECT_EQ(out, "\x1b[31mError\x1b[0m\n");
}

TEST(AnySinkColorTest, MalformedRangeIsIgnored) {
    struct RangeFormatter final : ylog::Formatter {
        std::pair<std::size_t, std::size_t> range;

        explicit RangeFormatter(std::pair<std::size_t, std::size_t> marked)
            : range(marked) {}

        auto format(const ylog::Message&, std::string& dest) -> void override {
            dest.append("abcdef\n");
        }
        auto clone() const -> std::unique_ptr<ylog::Formatter> override {
            return std::make_unique<RangeFormatter>(*this);
        }
        auto pattern() const -> std::string_view override { return ""; }
        auto color_range() const -> std::pair<std::size_t, std::size_t> override { return range; }
    };

    for (auto range :
         {std::pair<std::size_t, std::size_t>{4, 1}, std::pair<std::size_t, std::size_t>{0, 99}}) {
        auto out = std::string{};
        auto any = ylog::AnySink(ColoringCaptureSink{&out, ylog::ColorMode::Always},
                                 std::make_unique<RangeFormatter>(range));
        any.write(ylog::Message(ylog::Level::Info, "n", {}, "msg"));
        EXPECT_EQ(out, "abcdef\n");
    }
}
