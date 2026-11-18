#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(RotatingFileSinkTest, WritesConfiguredFile) {
    static_assert(ylog::Sink<ylog::RotatingFileSink>);

    auto path = temp_test_path("basic");
    auto config = ylog::RotatingFileConfig{};
    config.filename = path.string();
    config.max_size = 1024;
    config.max_files = 2;

    auto sink = ylog::RotatingFileSink(config);
    sink.write("hello\n");
    sink.flush();

    EXPECT_EQ(read_all(path), "hello\n");
    std::filesystem::remove(path);
}

TEST(RotatingFileSinkTest, ValidatesConfiguration) {
    auto path = temp_test_path("config");

    auto zero_size = ylog::RotatingFileConfig{};
    zero_size.filename = path.string();
    zero_size.max_size = 0;
    try {
        auto sink = ylog::RotatingFileSink(zero_size);
        FAIL() << "max_size == 0 should be rejected";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string_view(error.what()).find("max_size"), std::string_view::npos);
    }

    auto too_many = ylog::RotatingFileConfig{};
    too_many.filename = path.string();
    too_many.max_files = 200001;
    try {
        auto sink = ylog::RotatingFileSink(too_many);
        FAIL() << "max_files above 200000 should be rejected";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string_view(error.what()).find("max_files"), std::string_view::npos);
    }
}

TEST(RotatingFileSinkTest, AppendsByDefaultAndCanTruncate) {
    auto path = temp_test_path("modes");
    {
        auto seed = std::ofstream(path, std::ios::binary);
        seed << "old\n";
    }

    auto append_config = ylog::RotatingFileConfig{};
    append_config.filename = path.string();
    auto append_sink = ylog::RotatingFileSink(append_config);
    append_sink.write("new\n");
    append_sink.flush();
    EXPECT_EQ(read_all(path), "old\nnew\n");

    auto truncate_config = ylog::RotatingFileConfig{};
    truncate_config.filename = path.string();
    truncate_config.truncate_on_open = true;
    auto truncate_sink = ylog::RotatingFileSink(truncate_config);
    truncate_sink.write("fresh\n");
    truncate_sink.flush();
    EXPECT_EQ(read_all(path), "fresh\n");

    std::filesystem::remove(path);
}

TEST(RotatingFileSinkTest, CreatesParentDirectories) {
    auto root = temp_test_path("dirs");
    auto path = root / "nested" / "logs" / "app.log";

    auto config = ylog::RotatingFileConfig{};
    config.filename = path.string();
    auto sink = ylog::RotatingFileSink(config);
    sink.write("ok\n");
    sink.flush();

    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(read_all(path), "ok\n");
    std::filesystem::remove_all(root);
}

TEST(RotatingFileSinkTest, ReportsHotPathFailureToHandler) {
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "/dev/full is unavailable";
    }

    std::string reported;
    auto config = ylog::RotatingFileConfig{};
    config.filename = "/dev/full";
    config.err_handler = [&reported](std::string_view message) { reported = std::string(message); };

    auto sink = ylog::RotatingFileSink(config);
    EXPECT_NO_THROW({
        sink.write("hello\n");
        sink.flush();
    });
    EXPECT_NE(reported.find("flush"), std::string::npos);
}

TEST(RotatingFileSinkTest, DefaultErrorHandlerIsRateLimited) {
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "/dev/full is unavailable";
    }

    auto captured = std::ostringstream{};
    auto* original = std::cerr.rdbuf();
    std::cerr.rdbuf(captured.rdbuf());

    auto config = ylog::RotatingFileConfig{};
    config.filename = "/dev/full";
    auto sink = ylog::RotatingFileSink(config);
    sink.write("first\n");
    sink.flush();
    sink.write("second\n");
    sink.flush();

    std::cerr.rdbuf(original);
    EXPECT_EQ(std::ranges::count(captured.str(), '\n'), 1);
}

TEST(RotatingFileSinkTest, ReportsOpenFailureAtConfiguration) {
    auto directory = temp_test_path("open-failure");
    std::filesystem::create_directories(directory);

    auto config = ylog::RotatingFileConfig{};
    config.filename = directory.string();
    try {
        auto sink = ylog::RotatingFileSink(config);
        FAIL() << "opening a directory should be rejected at configuration";
    } catch (const std::invalid_argument& error) {
        // 配置期失败与校验失败共用一个类型前缀；文案是公开契约的一部分。
        auto expected = std::format("RotatingFileSink: failed to open '{}': ", directory.string());
        EXPECT_NE(std::string_view(error.what()).find(expected), std::string_view::npos)
            << error.what();
    }

    std::filesystem::remove_all(directory);
}

TEST(RotatingFileSinkTest, RotatesWhenSizeExceededAndWritesTriggeringLine) {
    auto directory = temp_test_path("rotate-basic");
    std::filesystem::create_directories(directory);
    auto active = directory / "app.log";
    auto backup = directory / "app.1.log";

    auto config = ylog::RotatingFileConfig{};
    config.filename = active.string();
    config.max_size = 8;
    config.max_files = 1;

    auto sink = ylog::RotatingFileSink(config);
    sink.write("12345");
    sink.write("678");
    sink.flush();
    EXPECT_EQ(read_all(active), "12345678");

    sink.write("9");
    sink.flush();

    EXPECT_EQ(read_all(active), "9");
    EXPECT_EQ(read_all(backup), "12345678");
    std::filesystem::remove_all(directory);
}

TEST(RotatingFileSinkTest, KeepsOnlyConfiguredBackups) {
    auto directory = temp_test_path("retention");
    std::filesystem::create_directories(directory);
    auto active = directory / "app.log";

    auto config = ylog::RotatingFileConfig{};
    config.filename = active.string();
    config.max_size = 4;
    config.max_files = 2;

    auto sink = ylog::RotatingFileSink(config);
    sink.write("aaaa");
    sink.write("b");
    sink.write("bbb");
    sink.write("c");
    sink.write("ddd");
    sink.write("e");
    sink.flush();

    EXPECT_EQ(read_all(active), "e");
    EXPECT_EQ(read_all(directory / "app.1.log"), "cddd");
    EXPECT_EQ(read_all(directory / "app.2.log"), "bbbb");
    EXPECT_FALSE(std::filesystem::exists(directory / "app.3.log"));
    std::filesystem::remove_all(directory);
}

TEST(RotatingFileSinkTest, BackupNameTreatsLeadingDotAsPartOfStem) {
    auto directory = temp_test_path("dot-name");
    std::filesystem::create_directories(directory);
    auto active = directory / ".log";

    auto config = ylog::RotatingFileConfig{};
    config.filename = active.string();
    config.max_size = 2;
    config.max_files = 1;

    auto sink = ylog::RotatingFileSink(config);
    sink.write("ab");
    sink.write("c");
    sink.flush();

    EXPECT_EQ(read_all(active), "c");
    EXPECT_EQ(read_all(directory / ".log.1"), "ab");
    std::filesystem::remove_all(directory);
}

TEST(RotatingFileSinkTest, RotateOnOpenMovesOversizedFile) {
    auto directory = temp_test_path("rotate-on-open");
    std::filesystem::create_directories(directory);
    auto active = directory / "app.log";
    {
        auto seed = std::ofstream(active, std::ios::binary);
        seed << "old\n";
    }

    auto config = ylog::RotatingFileConfig{};
    config.filename = active.string();
    config.max_size = 2;
    config.max_files = 1;
    config.rotate_on_open = true;

    auto sink = ylog::RotatingFileSink(config);
    sink.write("x");
    sink.flush();

    EXPECT_EQ(read_all(active), "x");
    EXPECT_EQ(read_all(directory / "app.1.log"), "old\n");
    std::filesystem::remove_all(directory);
}

TEST(RotatingFileSinkTest, RotateOnOpenIgnoresFileAtLimit) {
    auto directory = temp_test_path("rotate-on-open-limit");
    std::filesystem::create_directories(directory);
    auto active = directory / "app.log";
    {
        auto seed = std::ofstream(active, std::ios::binary);
        seed << "1234";
    }

    auto config = ylog::RotatingFileConfig{};
    config.filename = active.string();
    config.max_size = 4;
    config.max_files = 1;
    config.rotate_on_open = true;

    auto sink = ylog::RotatingFileSink(config);
    EXPECT_EQ(read_all(active), "1234");
    EXPECT_FALSE(std::filesystem::exists(directory / "app.1.log"));

    sink.write("x");
    sink.flush();
    EXPECT_EQ(read_all(directory / "app.1.log"), "1234");
    EXPECT_EQ(read_all(active), "x");
    std::filesystem::remove_all(directory);
}

TEST(RotatingFileSinkTest, ReportsRotationFailureToHandler) {
    auto directory = temp_test_path("rotate-failure");
    std::filesystem::create_directories(directory);
    auto active = directory / "app.log";
    auto blocking_backup = directory / "app.1.log";
    std::filesystem::create_directories(blocking_backup);
    {
        auto blocker = std::ofstream(blocking_backup / "busy", std::ios::binary);
        blocker << "keep";
    }

    std::string reported;
    auto config = ylog::RotatingFileConfig{};
    config.filename = active.string();
    config.max_size = 2;
    config.max_files = 1;
    config.err_handler = [&reported](std::string_view message) { reported = std::string(message); };

    auto sink = ylog::RotatingFileSink(config);
    EXPECT_NO_THROW({
        sink.write("ab");
        sink.write("c");
        sink.flush();
    });
    EXPECT_FALSE(reported.empty());
    std::filesystem::remove_all(directory);
}

TEST(RotatingFileSinkTest, NoBackupsWhenMaxFilesIsZero) {
    auto directory = temp_test_path("no-backups");
    std::filesystem::create_directories(directory);
    auto active = directory / "app.log";

    auto config = ylog::RotatingFileConfig{};
    config.filename = active.string();
    config.max_size = 4;
    config.max_files = 0;

    auto sink = ylog::RotatingFileSink(config);
    sink.write("aaaa");
    sink.write("b");
    sink.flush();

    EXPECT_EQ(read_all(active), "b");
    EXPECT_FALSE(std::filesystem::exists(directory / "app.1.log"));
    std::filesystem::remove_all(directory);
}
