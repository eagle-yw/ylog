#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(OstreamSinkTest, WritesCompleteLineToTheStream) {
    static_assert(ylog::Sink<ylog::OstreamSink>);

    auto captured = std::ostringstream{};
    auto sink = ylog::OstreamSink(ylog::OstreamConfig{.stream = &captured});

    sink.write("alpha\n");
    sink.write("beta\n");
    sink.flush();

    EXPECT_EQ(captured.str(), "alpha\nbeta\n");
}

TEST(OstreamSinkTest, RejectsNullStreamAtConfiguration) {
    EXPECT_THROW({ auto sink = ylog::OstreamSink(ylog::OstreamConfig{}); }, std::invalid_argument);
}

TEST(OstreamSinkTest, ColorModeIsResolvedAtConfiguration) {
    auto captured = std::ostringstream{};

    auto defaults = ylog::OstreamSink(ylog::OstreamConfig{.stream = &captured});
    auto automatic = ylog::OstreamSink(
        ylog::OstreamConfig{.stream = &captured, .color = ylog::ColorMode::Automatic});
    auto always = ylog::OstreamSink(
        ylog::OstreamConfig{.stream = &captured, .color = ylog::ColorMode::Always});

    EXPECT_EQ(defaults.color_mode(), ylog::ColorMode::Never);
    // 非终端 stream（含 ostringstream）在 Automatic 下不着色。
    EXPECT_EQ(automatic.color_mode(), ylog::ColorMode::Never);
    EXPECT_EQ(always.color_mode(), ylog::ColorMode::Always);
}

TEST(OstreamSinkTest, ReportsWriteFailureOnce) {
    auto failures = std::vector<std::string>{};
    auto failing = FailingStreambuf{};
    auto stream = std::ostream(&failing);

    auto sink = ylog::OstreamSink(ylog::OstreamConfig{
        .stream = &stream,
        .err_handler = [&failures](std::string_view message) { failures.emplace_back(message); },
    });

    EXPECT_NO_THROW(sink.write("first\n"));
    EXPECT_NO_THROW(sink.write("second\n"));
    EXPECT_EQ(failures.size(), 1);
}

TEST(StdoutSinkTest, WritesLineVerbatimWithDefaults) {
    static_assert(ylog::Sink<ylog::StdoutSink>);

    auto captured = std::ostringstream{};
    auto* original = std::cout.rdbuf(captured.rdbuf());

    auto sink = ylog::StdoutSink{};
    sink.write("plain line\n");
    sink.flush();

    std::cout.rdbuf(original);
    EXPECT_EQ(captured.str(), "plain line\n");
}

TEST(StdoutSinkTest, ReportsConfiguredColorMode) {
    auto never = ylog::StdoutSink(ylog::StdoutConfig{.color = ylog::ColorMode::Never});
    auto always = ylog::StdoutSink(ylog::StdoutConfig{.color = ylog::ColorMode::Always});

    EXPECT_EQ(never.color_mode(), ylog::ColorMode::Never);
    EXPECT_EQ(always.color_mode(), ylog::ColorMode::Always);
}

#if !defined(_WIN32)
TEST(StdoutSinkTest, AutomaticResolvesToNeverWhenStdoutIsRedirected) {
    auto path = temp_test_path("stdout-redirect");
    auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_NE(descriptor, -1) << "cannot open " << path.string();
    auto saved = ::dup(STDOUT_FILENO);
    ASSERT_NE(saved, -1);
    ASSERT_NE(::dup2(descriptor, STDOUT_FILENO), -1);

    // 探测定型发生在构造期：重定向之后再构造的 sink 应当已经知道不该上色。
    auto mode = ylog::StdoutSink{}.color_mode();

    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);
    ::close(descriptor);
    std::filesystem::remove(path);

    EXPECT_EQ(mode, ylog::ColorMode::Never);
}
#endif

TEST(StdoutSinkTest, ReportsWriteFailureOnce) {
    auto failures = std::vector<std::string>{};
    auto config = ylog::StdoutConfig{};
    config.err_handler = [&failures](std::string_view message) { failures.emplace_back(message); };

    auto failing = FailingStreambuf{};
    auto* original = std::cout.rdbuf(&failing);

    auto sink = ylog::StdoutSink(config);
    EXPECT_NO_THROW(sink.write("first\n"));
    EXPECT_NO_THROW(sink.write("second\n"));

    std::cout.rdbuf(original);
    std::cout.clear();
    EXPECT_EQ(failures.size(), 1);
}
