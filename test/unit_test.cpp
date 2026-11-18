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

namespace {

auto temp_test_path(std::string_view name) -> std::filesystem::path {
    static std::atomic<std::uint64_t> counter{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / std::format("ylog-rotating-{}-{}-{}", name, stamp, counter.fetch_add(1));
}

auto read_all(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

auto local_time_point(int year, int month, int day, int hour, int minute, int second)
    -> std::chrono::system_clock::time_point {
    auto value = std::tm{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    value.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&value));
}

} // namespace

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
    EXPECT_THROW({ auto sink = ylog::RotatingFileSink(config); }, std::invalid_argument);

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

    auto logger = ylog::Logger("file-integration");
    logger.set_level(ylog::Level::Info);
    logger.append_sink(ylog::RotatingFileSink(rotating_config), std::move(first_formatter));
    logger.append_sink(ylog::DailyFileSink(daily_config), std::move(second_formatter));
    logger.info({}, "msg");
    logger.flush();

    EXPECT_EQ(read_all(rotating_path), "FIRST|msg\n");
    EXPECT_EQ(read_all(directory / "daily_2026-11-18.log"), "SECOND|msg\n");
    std::filesystem::remove_all(directory);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
