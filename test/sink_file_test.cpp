#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(FileSinkTest, RejectsEmptyFilenameAtConfiguration) {
    static_assert(ylog::Sink<ylog::FileSink>);

    EXPECT_THROW({ auto sink = ylog::FileSink(ylog::FileConfig{}); }, std::invalid_argument);
}

TEST(FileSinkTest, RejectsUnopenablePathAtConfiguration) {
    auto directory = temp_test_path("file-open-failure");
    auto config = ylog::FileConfig{.filename = (directory / "app.log").string()};

    EXPECT_THROW({ auto sink = ylog::FileSink(config); }, std::invalid_argument);
}

TEST(FileSinkTest, AppendsByDefaultAndTruncatesOnRequest) {
    auto directory = temp_test_path("file-append");
    std::filesystem::create_directories(directory);
    auto path = directory / "app.log";

    {
        auto sink = ylog::FileSink(ylog::FileConfig{.filename = path.string()});
        sink.write("first\n");
        sink.flush();
    }
    {
        auto sink = ylog::FileSink(ylog::FileConfig{.filename = path.string()});
        sink.write("second\n");
        sink.flush();
    }
    EXPECT_EQ(read_all(path), "first\nsecond\n");

    {
        auto sink =
            ylog::FileSink(ylog::FileConfig{.filename = path.string(), .truncate_on_open = true});
        sink.write("third\n");
        sink.flush();
    }
    EXPECT_EQ(read_all(path), "third\n");

    std::filesystem::remove_all(directory);
}

TEST(FileSinkTest, ReportsHotPathFailureOnce) {
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "/dev/full is unavailable";
    }

    auto failures = 0;
    auto config = ylog::FileConfig{.filename = "/dev/full"};
    config.err_handler = [&failures](std::string_view) { ++failures; };

    auto sink = ylog::FileSink(config);
    EXPECT_NO_THROW({
        sink.write("first\n");
        sink.flush();
        sink.write("second\n");
        sink.flush();
    });
    EXPECT_EQ(failures, 1);
}

TEST(FileSinkIntegrationTest, WorksWithLoggerAndCustomFlags) {
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

    auto directory = temp_test_path("logger-integration");
    std::filesystem::create_directories(directory);
    auto rotating_path = directory / "rotating.log";
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));

    auto rotating_config = ylog::RotatingFileConfig{};
    rotating_config.filename = rotating_path.string();
    auto daily_config = ylog::DailyFileConfig{};
    daily_config.filename = (directory / "daily.log").string();
    daily_config.clock = [now] { return *now; };

    auto first_formatter = std::make_unique<ylog::PatternFormatter>("%x|%v");
    first_formatter->add_flag('x', First{});
    auto second_formatter = std::make_unique<ylog::PatternFormatter>("%x|%v");
    second_formatter->add_flag('x', Second{});

    auto logger = ylog::Logger(ylog::LoggerConfig{
        .name = "file-integration",
        .sinks = ylog::make_sinks(
            ylog::AnySink{ylog::RotatingFileSink(rotating_config), std::move(first_formatter)},
            ylog::AnySink{ylog::DailyFileSink(daily_config), std::move(second_formatter)}),
    });
    logger.info({}, "msg");
    logger.flush();

    EXPECT_EQ(read_all(rotating_path), "FIRST|msg\n");
    EXPECT_EQ(read_all(directory / "daily_2026-11-18.log"), "SECOND|msg\n");
    std::filesystem::remove_all(directory);
}
