#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(CallbackSinkTest, ReceivesTheSameLineAsOstreamSink) {
    static_assert(ylog::Sink<ylog::CallbackSink>);

    auto captured = std::ostringstream{};
    auto from_callback = std::string{};
    auto config = ylog::CallbackConfig{};
    config.fn = [&from_callback](std::string_view line) { from_callback = std::string(line); };

    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "callback-parity",
        .level = ylog::Level::Warning,
        .sinks = ylog::make_sinks(
            ylog::AnySink{ylog::CallbackSink(config)},
            ylog::AnySink{ylog::OstreamSink(ylog::OstreamConfig{.stream = &captured})}),
    });

    logger.warning({}, "same {}", "line");

    EXPECT_EQ(from_callback, captured.str());
    EXPECT_TRUE(from_callback.ends_with("same line\n")) << from_callback;
}

TEST(CallbackSinkTest, RejectsEmptyCallbackAtConfiguration) {
    EXPECT_THROW(
        { auto sink = ylog::CallbackSink(ylog::CallbackConfig{}); }, std::invalid_argument);
}

TEST(CallbackSinkTest, ReportsThrownCallbackOnceAndKeepsLogging) {
    auto reports = std::atomic<int>{0};
    auto store = ylog::RingBufferStore(4);
    auto config = ylog::CallbackConfig{};
    config.fn = [](std::string_view line) {
        if (line.starts_with("boom")) {
            throw std::runtime_error("callback failed");
        }
    };
    config.err_handler = [&reports](std::string_view) {
        reports.fetch_add(1, std::memory_order_relaxed);
    };

    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "callback-throws",
        .sinks = ylog::make_sinks(ylog::AnySink{ylog::CallbackSink(config), "%v"},
                                  ylog::AnySink{ylog::RingBufferSink(store), "%v"}),
    });

    EXPECT_NO_THROW(logger.info({}, "boom"));
    EXPECT_EQ(reports.load(), 1);

    // 回调失败只影响它自己那一次调用：logger 继续工作，其它 sink 照常收到行。
    EXPECT_NO_THROW(logger.info({}, "ok"));
    EXPECT_EQ(reports.load(), 1);
    EXPECT_EQ(store.last(4), (std::vector<std::string>{"boom\n", "ok\n"}));
}
