#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(DailyFileSinkTest, WritesDatedFileAtConstruction) {
    static_assert(ylog::Sink<ylog::DailyFileSink>);

    auto directory = temp_test_path("daily-basic");
    std::filesystem::create_directories(directory);
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));

    auto config = ylog::DailyFileConfig{};
    config.filename = (directory / "app.log").string();
    config.clock = [now] { return *now; };

    auto sink = ylog::DailyFileSink(config);
    sink.write("hello\n");
    sink.flush();

    EXPECT_EQ(read_all(directory / "app_2026-11-18.log"), "hello\n");
    std::filesystem::remove_all(directory);
}

TEST(DailyFileSinkTest, RotatesAtLocalMidnight) {
    auto directory = temp_test_path("daily-boundary");
    std::filesystem::create_directories(directory);
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 23, 59, 59));

    auto config = ylog::DailyFileConfig{};
    config.filename = (directory / "app.log").string();
    config.clock = [now] { return *now; };

    auto sink = ylog::DailyFileSink(config);
    sink.write("before\n");
    sink.flush();

    *now = local_time_point(2026, 11, 19, 0, 0, 0);
    sink.write("after\n");
    sink.flush();

    EXPECT_EQ(read_all(directory / "app_2026-11-18.log"), "before\n");
    EXPECT_EQ(read_all(directory / "app_2026-11-19.log"), "after\n");
    std::filesystem::remove_all(directory);
}

TEST(DailyFileSinkTest, AppendsByDefaultAndCanTruncate) {
    auto directory = temp_test_path("daily-modes");
    std::filesystem::create_directories(directory);
    auto dated = directory / "app_2026-11-18.log";
    {
        auto seed = std::ofstream(dated, std::ios::binary);
        seed << "old\n";
    }
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));

    auto append_config = ylog::DailyFileConfig{};
    append_config.filename = (directory / "app.log").string();
    append_config.clock = [now] { return *now; };
    auto append_sink = ylog::DailyFileSink(append_config);
    append_sink.write("new\n");
    append_sink.flush();
    EXPECT_EQ(read_all(dated), "old\nnew\n");

    auto truncate_config = ylog::DailyFileConfig{};
    truncate_config.filename = (directory / "app.log").string();
    truncate_config.clock = [now] { return *now; };
    truncate_config.truncate_on_open = true;
    auto truncate_sink = ylog::DailyFileSink(truncate_config);
    truncate_sink.write("fresh\n");
    truncate_sink.flush();
    EXPECT_EQ(read_all(dated), "fresh\n");

    std::filesystem::remove_all(directory);
}

TEST(DailyFileSinkTest, CreatesParentDirectories) {
    auto directory = temp_test_path("daily-dirs");
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));

    auto config = ylog::DailyFileConfig{};
    config.filename = (directory / "nested" / "logs" / "app.log").string();
    config.clock = [now] { return *now; };
    auto sink = ylog::DailyFileSink(config);
    sink.write("ok\n");
    sink.flush();

    EXPECT_EQ(read_all(directory / "nested" / "logs" / "app_2026-11-18.log"), "ok\n");
    std::filesystem::remove_all(directory);
}

TEST(DailyFileSinkTest, ValidatesConfiguration) {
    auto config = ylog::DailyFileConfig{};
    config.max_files = 200001;
    EXPECT_THROW({ auto sink = ylog::DailyFileSink(config); }, std::invalid_argument);

    auto no_clock = ylog::DailyFileConfig{};
    no_clock.clock = {};
    EXPECT_THROW({ auto sink = ylog::DailyFileSink(no_clock); }, std::invalid_argument);
}

TEST(DailyFileSinkTest, ReportsOpenFailureAtConfiguration) {
    auto directory = temp_test_path("daily-open-failure");
    // 用同名目录占住当天的文件路径，打开必然失败。
    std::filesystem::create_directories(directory / "app_2026-11-18.log");
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));

    auto config = ylog::DailyFileConfig{};
    config.filename = (directory / "app.log").string();
    config.clock = [now] { return *now; };
    try {
        auto sink = ylog::DailyFileSink(config);
        FAIL() << "opening a directory should be rejected at configuration";
    } catch (const std::invalid_argument& error) {
        // 配置期失败与校验失败共用一个类型前缀；文案是公开契约的一部分。
        auto expected = std::format("DailyFileSink: failed to open '{}': ",
                                    (directory / "app_2026-11-18.log").string());
        EXPECT_NE(std::string_view(error.what()).find(expected), std::string_view::npos)
            << error.what();
    }

    std::filesystem::remove_all(directory);
}

TEST(DailyFileSinkTest, ReportsHotPathFailureToHandler) {
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "/dev/full is unavailable";
    }

    auto directory = temp_test_path("daily-error");
    std::filesystem::create_directories(directory);
    auto dated = directory / "app_2026-11-18.log";
    auto error = std::error_code{};
    std::filesystem::create_symlink("/dev/full", dated, error);
    if (error) {
        GTEST_SKIP() << "symlinks are unavailable";
    }
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));

    std::string reported;
    auto config = ylog::DailyFileConfig{};
    config.filename = (directory / "app.log").string();
    config.clock = [now] { return *now; };
    config.err_handler = [&reported](std::string_view message) { reported = std::string(message); };

    auto sink = ylog::DailyFileSink(config);
    EXPECT_NO_THROW({
        sink.write("hello\n");
        sink.flush();
    });
    EXPECT_FALSE(reported.empty());
    std::filesystem::remove_all(directory);
}

TEST(DailyFileSinkTest, RetainsOnlyRecentCurrentRunFiles) {
    auto directory = temp_test_path("daily-retention");
    std::filesystem::create_directories(directory);
    {
        auto historical = std::ofstream(directory / "app_2020-01-01.log", std::ios::binary);
        historical << "old-run\n";
    }
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));

    auto config = ylog::DailyFileConfig{};
    config.filename = (directory / "app.log").string();
    config.max_files = 2;
    config.clock = [now] { return *now; };

    auto sink = ylog::DailyFileSink(config);
    sink.write("day1\n");
    *now = local_time_point(2026, 11, 19, 10, 0, 0);
    sink.write("day2\n");
    *now = local_time_point(2026, 11, 20, 10, 0, 0);
    sink.write("day3\n");
    sink.flush();

    EXPECT_FALSE(std::filesystem::exists(directory / "app_2026-11-18.log"));
    EXPECT_TRUE(std::filesystem::exists(directory / "app_2026-11-19.log"));
    EXPECT_TRUE(std::filesystem::exists(directory / "app_2026-11-20.log"));
    EXPECT_EQ(read_all(directory / "app_2020-01-01.log"), "old-run\n");
    std::filesystem::remove_all(directory);
}

TEST(DailyFileSinkTest, NoRetentionWhenMaxFilesIsZero) {
    auto directory = temp_test_path("daily-zero-retention");
    std::filesystem::create_directories(directory);
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));

    auto config = ylog::DailyFileConfig{};
    config.filename = (directory / "app.log").string();
    config.max_files = 0;
    config.clock = [now] { return *now; };

    auto sink = ylog::DailyFileSink(config);
    sink.write("day1\n");
    *now = local_time_point(2026, 11, 19, 10, 0, 0);
    sink.write("day2\n");
    *now = local_time_point(2026, 11, 20, 10, 0, 0);
    sink.write("day3\n");
    sink.flush();

    EXPECT_TRUE(std::filesystem::exists(directory / "app_2026-11-18.log"));
    EXPECT_TRUE(std::filesystem::exists(directory / "app_2026-11-19.log"));
    EXPECT_TRUE(std::filesystem::exists(directory / "app_2026-11-20.log"));
    std::filesystem::remove_all(directory);
}

TEST(DailyFileSinkTest, ReportsRetentionFailureToHandler) {
    auto directory = temp_test_path("daily-retention-failure");
    std::filesystem::create_directories(directory);
    auto now = std::make_shared<std::chrono::system_clock::time_point>(
        local_time_point(2026, 11, 18, 10, 0, 0));
    std::string reported;

    auto config = ylog::DailyFileConfig{};
    config.filename = (directory / "app.log").string();
    config.max_files = 1;
    config.clock = [now] { return *now; };
    config.err_handler = [&reported](std::string_view message) { reported = std::string(message); };

    auto sink = ylog::DailyFileSink(config);
    sink.write("day1\n");
    sink.flush();

    auto old_file = directory / "app_2026-11-18.log";
    std::filesystem::remove(old_file);
    std::filesystem::create_directories(old_file);
    {
        auto blocker = std::ofstream(old_file / "busy", std::ios::binary);
        blocker << "keep";
    }

    *now = local_time_point(2026, 11, 19, 10, 0, 0);
    sink.write("day2\n");
    sink.flush();

    EXPECT_NE(reported.find("remove old daily file"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(old_file / "busy"));
    std::filesystem::remove_all(directory);
}
