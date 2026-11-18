#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;

// 测试夹具与探针行解析：放文件级匿名命名空间，让早于各 TEST 段的用例也能用。
namespace {

// 把 worker 钉在一次 write 上的闸门：write 先放行 entered，再等 release。
struct SinkGate {
    std::counting_semaphore<64> entered{0};
    std::counting_semaphore<64> release{0};
};

struct BlockingSink {
    SinkGate* gate = nullptr;

    auto write(std::string_view) -> void {
        gate->entered.release();
        gate->release.acquire();
    }
    auto flush() -> void {}
};

struct ThrowingSink {
    auto write(std::string_view) -> void { throw std::runtime_error("boom"); }
    auto flush() -> void {}
};

// 测试自己造的行：生产者号-序号-固定载荷。按字段解析而不是按下标切字符串，
// 断言失败时才说得清是哪个字段坏了。
struct ProbeLine {
    int producer = 0;
    int sequence = 0;
    std::string payload;
};

auto parse_probe_line(std::string_view line) -> std::optional<ProbeLine> {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }

    auto to_int = [](std::string_view text) -> std::optional<int> {
        if (text.empty()) {
            return std::nullopt;
        }
        auto value = 0;
        for (auto ch : text) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
            value = value * 10 + (ch - '0');
        }
        return value;
    };

    auto fields = std::vector<std::string_view>{};
    for (auto part : std::views::split(line, '-')) {
        fields.emplace_back(part.begin(), part.end());
    }
    if (fields.size() != 3) {
        return std::nullopt;
    }
    auto producer = to_int(fields[0]);
    auto sequence = to_int(fields[1]);
    if (!producer || !sequence) {
        return std::nullopt;
    }
    return ProbeLine{*producer, *sequence, std::string(fields[2])};
}

// 每个用例独占临时目录：测试不往工作树里写产物。
auto temp_test_path(std::string_view name) -> std::filesystem::path {
    static std::atomic<std::uint64_t> counter{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           std::format("ylog-rotating-{}-{}-{}", name, stamp, counter.fetch_add(1));
}

auto read_all(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// 一点字节都不接受的底层：让任何写入都以 badbit 失败，用来验证热路径的失败上报。
struct FailingStreambuf : std::streambuf {
    auto xsputn(const char*, std::streamsize) -> std::streamsize override { return 0; }
    auto overflow(int_type) -> int_type override { return traits_type::eof(); }
};

// 只数 flush 次数的 sink：flush_on 的验收要回答「是谁触发了 flush」。
struct FlushCountingSink {
    std::atomic<int>* flushes;
    auto write(std::string_view) -> void {}
    auto flush() -> void { flushes->fetch_add(1, std::memory_order_relaxed); }
};

// flush 卡在闸门上的 sink：worker 停在 flush 里时，生产者必须已经返回。
struct GatedFlushSink {
    std::atomic<bool>* entered;
    std::counting_semaphore<1>* release;

    auto write(std::string_view) -> void {}
    auto flush() -> void {
        *entered = true;
        release->acquire();
    }
};

// 收集行的 sink：断言「收到了什么字节」的用例共用它。
struct CaptureSink {
    std::string* out;
    auto write(std::string_view line) -> void { out->append(line); }
};

// 声明自己接受颜色的捕获 sink：用于不经过具体 sink 的上色用例。
struct ColoringCaptureSink {
    std::string* out;
    ylog::ColorMode mode;

    auto write(std::string_view line) -> void { out->append(line); }
    auto color_mode() const -> ylog::ColorMode { return mode; }
};

// 轮询等待，避免用固定 sleep 猜 worker 的进度。
auto wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}  // namespace

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

TEST(SinkTest, FormatterSeamRejectsEmptyFormatter) {
    auto captured = std::string{};
    auto any =
        ylog::AnySink(CaptureSink{&captured}, std::make_unique<ylog::PatternFormatter>("%v"));

    EXPECT_THROW(any.set_formatter(nullptr), std::invalid_argument);

    // 拒绝之后原 formatter 仍在：这次违约不改变 sink 的可用状态。
    any.write(ylog::Message(ylog::Level::Info, "n", {}, "still"));
    EXPECT_EQ(captured, "still\n");
}

TEST(LoggerTest, ShouldLogFollowsLevel) {
    auto logger = ylog::Logger("should-log");

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

TEST(LoggerTest, SinksExposeEveryAppendedSink) {
    auto first = std::string{};
    auto second = std::string{};
    auto logger = ylog::Logger("sinks");
    logger.set_level(ylog::Level::Info);

    // Logger 构造时已经装好一个默认 StdoutSink。
    auto initial = logger.sinks().size();
    EXPECT_EQ(initial, 1);

    logger.append_sink(CaptureSink{&first}, "%v");
    logger.append_sink(CaptureSink{&second}, "%v");
    EXPECT_EQ(logger.sinks().size(), initial + 2);

    // 元素可配置：单个 sink 的 pattern 只影响自己。
    logger.sinks()[initial].set_pattern("%l: %v");
    logger.sinks()[initial + 1].set_pattern("[%v]");

    logger.info({}, "hello");
    logger.flush();

    EXPECT_EQ(first, "Info: hello\n");
    EXPECT_EQ(second, "[hello]\n");
}

TEST(LoggerTest, SetFormatterFansOutToEverySink) {
    auto first = std::string{};
    auto second = std::string{};
    auto logger = ylog::Logger("set-formatter");
    logger.set_level(ylog::Level::Info);
    logger.append_sink(CaptureSink{&first});
    logger.append_sink(CaptureSink{&second});
    auto first_index = logger.sinks().size() - 2;

    logger.set_formatter(std::make_unique<ylog::PatternFormatter>("%l|%v"));
    logger.info({}, "hello");
    EXPECT_EQ(first, "Info|hello\n");
    EXPECT_EQ(second, "Info|hello\n");

    // formatter 归属 sink：扇出的是各自的 clone，改一个不影响另一个。
    logger.sinks()[first_index].set_pattern("%v");
    logger.info({}, "again");
    EXPECT_EQ(first, "Info|hello\nagain\n");
    EXPECT_EQ(second, "Info|hello\nInfo|again\n");
}

TEST(LoggerTest, SetFormatterRejectsEmptyFormatter) {
    auto logger = ylog::Logger("empty-formatter");

    EXPECT_THROW(logger.set_formatter(nullptr), std::invalid_argument);
}

TEST(LoggerTest, FlushOnTriggersOnlyForConfiguredLevels) {
    auto flushes = std::atomic<int>{0};
    auto logger = ylog::Logger("flush-on");
    logger.set_level(ylog::Level::Trace);
    logger.append_sink(FlushCountingSink{&flushes});

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
    auto logger = ylog::Logger("flags");
    logger.set_level(ylog::Level::Info);
    std::string out;
    // 只用已知 flag: %L(短级别) %n(名称) %v(payload) %%(字面百分号)
    logger.append_sink(CaptureSink{&out}, "[%L] %n: %v 100%%");
    logger.info({}, "hello");

    EXPECT_EQ(out, "[I] flags: hello 100%\n");
}

TEST(FormatterTest, PerSinkPattern) {
    auto logger = ylog::Logger("multi");
    logger.set_level(ylog::Level::Info);
    std::string short_out, long_out;
    // 两个 sink 使用不同 pattern —— 移植自 spdlog 的核心设计
    logger.append_sink(CaptureSink{&short_out}, "%v");
    logger.append_sink(CaptureSink{&long_out}, "[%L] %v");
    logger.info({}, "same message");

    EXPECT_EQ(short_out, "same message\n");
    EXPECT_EQ(long_out, "[I] same message\n");
}

TEST(FormatterTest, SetPatternFanout) {
    auto logger = ylog::Logger("fanout");
    logger.set_level(ylog::Level::Info);
    std::string a, b;
    logger.append_sink(CaptureSink{&a}, "%v");
    logger.append_sink(CaptureSink{&b}, "%v");
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

    auto logger = ylog::Logger("custom-sinks");
    logger.set_level(ylog::Level::Info);
    std::string a, b;
    logger.append_sink(CaptureSink{&a}, std::move(first));
    logger.append_sink(CaptureSink{&b}, std::move(second));
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

    auto logger = ylog::Logger("fanout-custom");
    logger.set_level(ylog::Level::Info);
    std::string a, b;
    logger.append_sink(CaptureSink{&a}, std::move(first));
    logger.append_sink(CaptureSink{&b}, std::move(second));
    logger.set_pattern("[%x]");
    logger.info({}, "msg");

    EXPECT_EQ(a, "[C]\n");
    EXPECT_EQ(b, "[C]\n");
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

    auto logger = ylog::Logger("plain-formatter");
    logger.set_level(ylog::Level::Info);
    std::string out;
    logger.append_sink(CaptureSink{&out}, std::make_unique<PlainFormatter>());

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
    auto directory = temp_test_path("logger-file-sink");
    std::filesystem::create_directories(directory);
    auto path = directory / "sink.log";

    auto logger = ylog::Logger("file-test");
    logger.set_level(ylog::Level::Debug);
    logger.append_sink(ylog::FileSink(path.string()), "%v");

    logger.debug({}, "debug line");
    logger.info({}, "info line");
    logger.error({}, "error line");
    logger.flush();

    EXPECT_EQ(read_all(path), "debug line\ninfo line\nerror line\n");

    std::filesystem::remove_all(directory);
}

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

TEST(RingBufferStoreTest, RejectsZeroCapacityAtConfiguration) {
    EXPECT_THROW({ auto store = ylog::RingBufferStore(0); }, std::invalid_argument);
}

TEST(RingBufferStoreTest, KeepsTheMostRecentLinesInOrder) {
    auto store = ylog::RingBufferStore(3);
    for (int index = 0; index < 5; ++index) {
        store.push(std::format("line-{}\n", index));
    }

    EXPECT_EQ(store.capacity(), 3u);
    EXPECT_EQ(store.size(), 3u);

    auto recent = store.last(3);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent[0], "line-2\n");
    EXPECT_EQ(recent[1], "line-3\n");
    EXPECT_EQ(recent[2], "line-4\n");

    // n >= size() 时返回全部，顺序不变。
    EXPECT_EQ(store.last(10), recent);
}

TEST(RingBufferStoreTest, ConcurrentWritersAndReadersNeverTearALine) {
    auto store = ylog::RingBufferStore(64);
    constexpr auto producer_count = 4;
    constexpr auto per_producer = 200;
    auto stop_reading = std::atomic<bool>{false};
    auto torn_lines = std::atomic<int>{0};

    auto producers = std::vector<std::thread>{};
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&store, producer] {
            for (int sequence = 0; sequence < per_producer; ++sequence) {
                store.push(std::format("{}-{:04d}-{}", producer, sequence, std::string(40, 'x')));
            }
        });
    }

    auto reader = std::thread([&store, &stop_reading, &torn_lines] {
        while (!stop_reading.load()) {
            for (const auto& line : store.last(64)) {
                auto parsed = parse_probe_line(line);
                if (!parsed || parsed->payload != std::string(40, 'x')) {
                    torn_lines.fetch_add(1);
                }
            }
        }
    });

    for (auto& producer : producers) {
        producer.join();
    }
    stop_reading = true;
    reader.join();

    EXPECT_EQ(torn_lines.load(), 0);
    EXPECT_EQ(store.size(), 64u);
}

TEST(RingBufferSinkTest, PushesFormattedLinesIntoTheStore) {
    static_assert(ylog::Sink<ylog::RingBufferSink>);

    auto store = ylog::RingBufferStore(4);
    auto logger = ylog::Logger("ring-buffer");
    logger.set_level(ylog::Level::Info);
    logger.append_sink(ylog::RingBufferSink(store), "%v");

    logger.info({}, "one");
    logger.info({}, "two");
    logger.flush();

    auto lines = store.last(4);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "one\n");
    EXPECT_EQ(lines[1], "two\n");
}

TEST(CallbackSinkTest, ReceivesTheSameLineAsOstreamSink) {
    static_assert(ylog::Sink<ylog::CallbackSink>);

    auto captured = std::ostringstream{};
    auto from_callback = std::string{};
    auto config = ylog::CallbackConfig{};
    config.fn = [&from_callback](std::string_view line) { from_callback = std::string(line); };

    auto logger = ylog::Logger("callback-parity");
    logger.set_level(ylog::Level::Warning);
    logger.append_sink(ylog::CallbackSink(config));
    logger.append_sink(ylog::OstreamSink(ylog::OstreamConfig{.stream = &captured}));

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

    auto logger = ylog::Logger("callback-throws");
    logger.set_level(ylog::Level::Info);
    logger.append_sink(ylog::CallbackSink(config), "%v");
    logger.append_sink(ylog::RingBufferSink(store), "%v");

    EXPECT_NO_THROW(logger.info({}, "boom"));
    EXPECT_EQ(reports.load(), 1);

    // 回调失败只影响它自己那一次调用：logger 继续工作，其它 sink 照常收到行。
    EXPECT_NO_THROW(logger.info({}, "ok"));
    EXPECT_EQ(reports.load(), 1);
    EXPECT_EQ(store.last(4), (std::vector<std::string>{"boom\n", "ok\n"}));
}

TEST(GenericSinkTest, FanOutKeepsAppendSinkOrder) {
    auto order = std::string{};
    auto store = ylog::RingBufferStore(4);
    auto logger = ylog::Logger("fan-out");
    logger.set_level(ylog::Level::Info);

    auto first = ylog::CallbackConfig{};
    first.fn = [&order](std::string_view) { order += "first;"; };
    auto second = ylog::CallbackConfig{};
    second.fn = [&order](std::string_view) { order += "second;"; };

    logger.append_sink(ylog::CallbackSink(first), "%v");
    logger.append_sink(ylog::RingBufferSink(store), "%v");
    logger.append_sink(ylog::CallbackSink(second), "%v");

    // 默认的 StdoutSink 仍在扇出里：新 sink 与它并存，各自都收到同一行。
    auto to_stdout = std::ostringstream{};
    auto* original = std::cout.rdbuf(to_stdout.rdbuf());
    logger.info({}, "msg");
    std::cout.rdbuf(original);

    EXPECT_EQ(order, "first;second;");
    EXPECT_EQ(store.last(1), std::vector<std::string>{"msg\n"});
    EXPECT_TRUE(to_stdout.str().ends_with("msg\n")) << to_stdout.str();
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

TEST(RegistryTest, DefaultLoggerIsRegistrySlot) {
    auto& registry = ylog::Registry::instance();
    auto& logger = ylog::default_logger();

    // 默认 logger 与 create("default") 是同一个实例，没有第二份同名实现（ADR-0010）。
    EXPECT_EQ(&logger, &registry.create("default"));
    EXPECT_EQ(&logger, registry.get("default"));
    EXPECT_EQ(logger.name(), ylog::default_logger_name);
}

TEST(RegistryTest, DefaultLoggerIsPermanent) {
    auto& registry = ylog::Registry::instance();
    auto* logger = &ylog::default_logger();

    EXPECT_FALSE(registry.drop("default"));
    EXPECT_TRUE(registry.contains("default"));
    EXPECT_EQ(&ylog::default_logger(), logger);

    registry.create("doomed");
    registry.drop_all();
    EXPECT_FALSE(registry.contains("doomed"));
    EXPECT_TRUE(registry.contains("default"));
    EXPECT_EQ(&ylog::default_logger(), logger);
}

TEST(RegistryTest, FreeFunctionsTargetDefaultSlot) {
    auto& registry = ylog::Registry::instance();

    ylog::set_level(ylog::Level::Warning);
    EXPECT_EQ(ylog::default_logger().level(), ylog::Level::Warning);

    // 按名字配置与自由函数作用于同一实例，两条路径可互换。
    EXPECT_TRUE(registry.set_level(ylog::default_logger_name, ylog::Level::Error));
    EXPECT_EQ(ylog::default_logger().level(), ylog::Level::Error);
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

TEST(SinkTest, NullSinkWritesNothingAnywhere) {
    static_assert(ylog::Sink<ylog::NullSink>);

    auto to_stdout = std::ostringstream{};
    auto to_stderr = std::ostringstream{};
    auto* saved_out = std::cout.rdbuf(to_stdout.rdbuf());
    auto* saved_err = std::cerr.rdbuf(to_stderr.rdbuf());

    auto sink = ylog::AnySink(ylog::NullSink{}, std::make_unique<ylog::PatternFormatter>("%v"));
    sink.write(ylog::Message(ylog::Level::Info, "n", {}, "dropped"));
    sink.flush();

    std::cout.rdbuf(saved_out);
    std::cerr.rdbuf(saved_err);
    EXPECT_TRUE(to_stdout.str().empty());
    EXPECT_TRUE(to_stderr.str().empty());
}

TEST(SinkTest, NullSinkDoesNotChangeOtherLoggerBehaviour) {
    auto output = std::string{};
    auto logger = ylog::Logger("null-sink");
    logger.set_level(ylog::Level::Info);
    logger.append_sink(ylog::NullSink{}, "%v");
    logger.append_sink(CaptureSink{&output}, "%v");

    logger.debug({}, "filtered");
    logger.info({}, "kept");
    logger.flush();

    EXPECT_EQ(output, "kept\n");
}

TEST(SinkTest, ColorModeIsAnOptionalSinkProtocol) {
    struct ColoringSink {
        auto write(std::string_view) -> void {}
        auto color_mode() const -> ylog::ColorMode { return ylog::ColorMode::Always; }
    };

    auto plain = ylog::AnySink(ylog::NullSink{});
    EXPECT_EQ(plain.color_mode(), ylog::ColorMode::Never);

    auto coloring = ylog::AnySink(ColoringSink{});
    EXPECT_EQ(coloring.color_mode(), ylog::ColorMode::Always);
}

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

namespace {

// 一条经过 OstreamSink 的完整输出：用来断言「哪些字节该出现、哪些不该」。
auto ostream_sink_output(ylog::Level level, ylog::ColorMode mode, std::string_view pattern)
    -> std::string {
    auto captured = std::ostringstream{};
    auto sink = ylog::OstreamSink(ylog::OstreamConfig{.stream = &captured, .color = mode});
    auto any = ylog::AnySink(std::move(sink), std::make_unique<ylog::PatternFormatter>(pattern));
    any.write(ylog::Message(level, "n", {}, "msg"));
    return captured.str();
}

}  // namespace

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

TEST(SinkConcurrencyTest, ConcurrentWritesKeepEveryLineIntact) {
    std::string output;
    auto sink = ylog::AnySink(CaptureSink{&output}, std::make_unique<ylog::PatternFormatter>("%v"));

    constexpr int thread_count = 4;
    constexpr int per_thread = 200;
    auto produce = [&sink](int tid) {
        for (int i = 0; i < per_thread; ++i) {
            auto payload = std::format("{}-{:04d}-{}", tid, i, std::string(40, 'x'));
            auto message = ylog::Message(ylog::Level::Info, "n", {}, payload);
            sink.write(message);
        }
    };

    auto pool = std::vector<std::thread>{};
    for (int tid = 0; tid < thread_count; ++tid) {
        pool.emplace_back(produce, tid);
    }
    for (auto& worker : pool) {
        worker.join();
    }

    auto seen = std::set<std::pair<int, int>>{};
    std::size_t pos = 0;
    while (pos < output.size()) {
        auto end = output.find('\n', pos);
        ASSERT_NE(end, std::string::npos) << "unterminated line at offset " << pos;
        auto line = std::string_view(output).substr(pos, end - pos);
        auto parsed = parse_probe_line(line);
        ASSERT_TRUE(parsed.has_value()) << "clobbered line: " << std::string(line);
        EXPECT_EQ(parsed->payload, std::string(40, 'x')) << "payload corrupted in " << line;
        EXPECT_TRUE(seen.emplace(parsed->producer, parsed->sequence).second)
            << "duplicate line " << line;
        pos = end + 1;
    }
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(thread_count) * per_thread);
}

TEST(SinkConcurrencyTest, FormattingDoesNotHoldTheOutputLock) {
    struct Gate {
        std::atomic<bool> in_write{false};
        std::atomic<bool> formatted_while_writing{false};
        std::counting_semaphore<2> release{0};
    };

    struct ProbeFormatter final : ylog::Formatter {
        Gate* gate;
        explicit ProbeFormatter(Gate* target)
            : gate(target) {}
        auto format(const ylog::Message&, std::string& dest) -> void override {
            if (gate->in_write.load()) {
                gate->formatted_while_writing = true;
            }
            dest.append("probe\n");
        }
        auto clone() const -> std::unique_ptr<ylog::Formatter> override {
            return std::make_unique<ProbeFormatter>(*this);
        }
        auto pattern() const -> std::string_view override { return ""; }
    };

    // 与文件级的 BlockingSink 不同：这里只需要「占住输出锁」，不关心进出配对。
    struct HeldOutputSink {
        Gate* gate;
        auto write(std::string_view) -> void {
            gate->in_write = true;
            gate->release.acquire();
        }
    };

    Gate gate;
    auto sink = ylog::AnySink(HeldOutputSink{&gate}, std::make_unique<ProbeFormatter>(&gate));
    auto message = ylog::Message(ylog::Level::Info, "n", {}, "msg");

    std::thread first([&sink, &message] { sink.write(message); });
    while (!gate.in_write.load()) {
        std::this_thread::yield();
    }

    std::thread second([&sink, &message] { sink.write(message); });

    // 若格式化落在输出锁内，第二个线程永远等不到格式化机会。
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (!gate.formatted_while_writing.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto formatted_in_parallel = gate.formatted_while_writing.load();

    gate.release.release(2);
    first.join();
    second.join();

    EXPECT_TRUE(formatted_in_parallel);
}

namespace {

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

}  // namespace

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

namespace {

// 异步组件的观察点：sink 侧记录行内容并通知，测试据此等待，不 sleep 轮询。
struct AsyncRecordingSink {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::string> lines;
    };

    std::shared_ptr<State> state;

    auto write(std::string_view line) -> void {
        {
            std::lock_guard lock(state->mutex);
            state->lines.emplace_back(line);
        }
        state->changed.notify_all();
    }

    auto flush() -> void {}

    auto wait_for(std::size_t count, std::chrono::milliseconds timeout) -> bool {
        std::unique_lock lock(state->mutex);
        return state->changed.wait_for(lock, timeout,
                                       [this, count] { return state->lines.size() >= count; });
    }

    auto snapshot() -> std::vector<std::string> {
        std::lock_guard lock(state->mutex);
        return state->lines;
    }
};

// 收集 err_handler 的上报内容；handler 可能从多个生产者线程并发调用。
struct ErrorReports {
    struct State {
        std::mutex mutex;
        std::vector<std::string> messages;
    };

    std::shared_ptr<State> state;

    auto handler() const -> ylog::ErrorHandler {
        auto captured = state;
        return [captured](std::string_view text) {
            std::lock_guard lock(captured->mutex);
            captured->messages.emplace_back(text);
        };
    }

    auto snapshot() const -> std::vector<std::string> {
        std::lock_guard lock(state->mutex);
        return state->messages;
    }
};

// 记录行内容后再卡住，用来把工作线程钉在一次 write 上。
struct GatedRecordingSink {
    std::shared_ptr<AsyncRecordingSink::State> state;
    std::counting_semaphore<64>* entered = nullptr;
    std::counting_semaphore<64>* release = nullptr;

    auto write(std::string_view line) -> void {
        AsyncRecordingSink{state}.write(line);
        entered->release();
        release->acquire();
    }

    auto flush() -> void {}
};

}  // namespace

TEST(AsyncLoggerTest, ConfigDefaultsMatchTheSpec) {
    auto config = ylog::AsyncLoggerConfig{.name = "async"};

    EXPECT_EQ(config.level, ylog::Level::Info);
    EXPECT_EQ(config.queue_capacity, std::size_t{8192});
    EXPECT_EQ(config.overflow_policy, ylog::OverflowPolicy::Block);
    EXPECT_TRUE(static_cast<bool>(config.err_handler));
}

TEST(AsyncLoggerTest, RejectsZeroQueueCapacity) {
    auto config = ylog::AsyncLoggerConfig{.name = "async", .queue_capacity = 0};

    EXPECT_THROW(ylog::AsyncLogger{config}, std::invalid_argument);
}

TEST(AsyncLoggerTest, RejectsUnknownOverflowPolicy) {
    auto config = ylog::AsyncLoggerConfig{.name = "async"};
    config.overflow_policy = static_cast<ylog::OverflowPolicy>(42);

    EXPECT_THROW(ylog::AsyncLogger{config}, std::invalid_argument);
}

TEST(AsyncLoggerTest, ExposesNameAndLevel) {
    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});

    EXPECT_EQ(logger.name(), "async");
    EXPECT_EQ(logger.level(), ylog::Level::Info);

    logger.set_level(ylog::Level::Warning);

    EXPECT_EQ(logger.level(), ylog::Level::Warning);
}

TEST(AsyncLoggerTest, ShouldLogFollowsLevel) {
    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async-should-log"});

    EXPECT_FALSE(logger.should_log(ylog::Level::Debug));
    EXPECT_TRUE(logger.should_log(ylog::Level::Info));

    logger.set_level(ylog::Level::Trace);
    EXPECT_TRUE(logger.should_log(ylog::Level::Trace));
    EXPECT_TRUE(logger.should_log(ylog::Level::Critical));
}

TEST(AsyncLoggerTest, SinksExposeEveryAppendedSink) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};
    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async-sinks"});

    auto initial = logger.sinks().size();
    EXPECT_EQ(initial, 1);

    logger.append_sink(view, "%v");
    EXPECT_EQ(logger.sinks().size(), initial + 1);
    EXPECT_EQ(logger.sinks()[initial].pattern(), "%v");
}

TEST(AsyncLoggerTest, SinksConfigureIndividually) {
    auto first = std::make_shared<AsyncRecordingSink::State>();
    auto second = std::make_shared<AsyncRecordingSink::State>();
    auto first_sink = AsyncRecordingSink{first};
    auto second_sink = AsyncRecordingSink{second};

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async-configure"});
    logger.append_sink(first_sink, "%v");
    logger.append_sink(second_sink, "%v");
    auto first_index = logger.sinks().size() - 2;

    // 扇出：配置期的 set_formatter 让每个 sink 拿到独立 clone。
    logger.set_formatter(std::make_unique<ylog::PatternFormatter>("%l|%v"));
    logger.info({}, "shared");
    ASSERT_TRUE(first_sink.wait_for(1, std::chrono::milliseconds(1000)));
    ASSERT_TRUE(second_sink.wait_for(1, std::chrono::milliseconds(1000)));
    EXPECT_EQ(first_sink.snapshot().back(), "Info|shared\n");
    EXPECT_EQ(second_sink.snapshot().back(), "Info|shared\n");

    // 逐 sink 配置：sinks() 交出的元素仍可改，改一个不影响另一个。
    logger.sinks()[first_index].set_pattern("%v");
    logger.info({}, "own");
    ASSERT_TRUE(wait_until([&first] {
        std::lock_guard lock(first->mutex);
        return first->lines.size() >= 2;
    }));
    ASSERT_TRUE(wait_until([&second] {
        std::lock_guard lock(second->mutex);
        return second->lines.size() >= 2;
    }));
    EXPECT_EQ(first_sink.snapshot().back(), "own\n");
    EXPECT_EQ(second_sink.snapshot().back(), "Info|own\n");
}

TEST(AsyncLoggerTest, FlushOnFlushesWithoutAnExplicitFlush) {
    auto flushes = std::atomic<int>{0};
    auto recorded = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{recorded};
    auto logger =
        ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async-flush-on", .queue_capacity = 8});
    logger.append_sink(view, "%v");
    logger.append_sink(FlushCountingSink{&flushes});

    // 默认 Off：消息照常投递，但日志路径一次都不 flush。
    EXPECT_EQ(logger.flush_level(), ylog::Level::Off);
    logger.error({}, "default is off");
    ASSERT_TRUE(view.wait_for(1, std::chrono::milliseconds(1000)));
    EXPECT_EQ(flushes.load(), 0);

    logger.flush_on(ylog::Level::Warning);
    logger.info({}, "below the threshold");
    EXPECT_EQ(flushes.load(), 0);

    logger.warning({}, "at the threshold");
    EXPECT_TRUE(wait_until([&flushes] { return flushes.load() > 0; }));
}

TEST(AsyncLoggerTest, FlushOnDoesNotBlockTheProducer) {
    auto entered = std::atomic<bool>{false};
    auto release = std::counting_semaphore<1>{0};
    auto producer_done = std::atomic<bool>{false};

    auto logger = ylog::AsyncLogger(
        ylog::AsyncLoggerConfig{.name = "async-flush-on-blocking", .queue_capacity = 4});
    logger.append_sink(GatedFlushSink{&entered, &release});
    logger.flush_on(ylog::Level::Info);

    // 队列装得下这三条，所以生产者只可能在「等 flush」上被挡住。
    auto producer = std::thread([&logger, &producer_done] {
        for (int index = 0; index < 3; ++index) {
            logger.info({}, "line {}", index);
        }
        producer_done = true;
    });

    auto entered_in_time = wait_until([&entered] { return entered.load(); });
    auto finished_while_flushing = producer_done.load();

    release.release();
    producer.join();
    logger.stop();

    EXPECT_TRUE(entered_in_time);
    EXPECT_TRUE(finished_while_flushing);
}

TEST(AsyncLoggerTest, DeliversMessageToItsSink) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
    logger.append_sink(view, "%v");
    logger.info({}, "hello {}", 42);

    ASSERT_TRUE(view.wait_for(1, std::chrono::milliseconds(1000)));

    auto lines = view.snapshot();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "hello 42\n");
}

TEST(AsyncLoggerTest, FiltersBelowThresholdBeforeEnqueue) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
    logger.append_sink(view, "%v");
    logger.set_level(ylog::Level::Warning);
    logger.info({}, "dropped");
    logger.warning({}, "kept");

    // 单工作线程按 FIFO 投递：看到 kept 时，若 dropped 曾入队必然已在其中。
    ASSERT_TRUE(view.wait_for(1, std::chrono::milliseconds(1000)));

    auto lines = view.snapshot();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "kept\n");
}

TEST(AsyncLoggerTest, ReturnsWithoutWaitingForSink) {
    auto gate = SinkGate{};
    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
    logger.append_sink(BlockingSink{&gate}, "%v");

    std::binary_semaphore returned{0};
    auto producer = std::jthread([&logger, &returned] {
        logger.log({}, ylog::Level::Info, "blocked");
        returned.release();
    });

    bool logged_without_waiting = returned.try_acquire_for(std::chrono::milliseconds(500));
    gate.release.release();

    EXPECT_TRUE(logged_without_waiting) << "log() 等到了 sink 写完，说明投递仍在调用线程上";
}

TEST(AsyncLoggerTest, ConcurrentProducersDeliverEveryLineIntact) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
    logger.append_sink(view, "%v");

    constexpr int thread_count = 4;
    constexpr int per_thread = 200;
    auto produce = [&logger](int tid) {
        for (int i = 0; i < per_thread; ++i) {
            logger.info({}, "{}-{:04d}-{}", tid, i, std::string(40, 'x'));
        }
    };

    auto pool = std::vector<std::jthread>{};
    for (int tid = 0; tid < thread_count; ++tid) {
        pool.emplace_back(produce, tid);
    }
    for (auto& worker : pool) {
        worker.join();
    }

    ASSERT_TRUE(view.wait_for(thread_count * per_thread, std::chrono::milliseconds(2000)));

    auto lines = view.snapshot();
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(thread_count) * per_thread);

    auto seen = std::set<std::pair<int, int>>{};
    for (const auto& line : lines) {
        auto parsed = parse_probe_line(line);
        ASSERT_TRUE(parsed.has_value()) << "clobbered line: " << line;
        EXPECT_EQ(parsed->payload, std::string(40, 'x')) << "payload corrupted in " << line;
        EXPECT_TRUE(seen.emplace(parsed->producer, parsed->sequence).second)
            << "duplicate line " << line;
    }
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(thread_count) * per_thread);
}

TEST(AsyncLoggerTest, DrainsQueueInOrderOnDestruction) {
    constexpr int message_count = 200;
    auto state = std::make_shared<AsyncRecordingSink::State>();
    {
        auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
        logger.append_sink(AsyncRecordingSink{state}, "%v");
        for (int i = 0; i < message_count; ++i) {
            logger.info({}, "line-{}", i);
        }
    }

    auto view = AsyncRecordingSink{state};
    auto lines = view.snapshot();
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(message_count));
    for (int i = 0; i < message_count; ++i) {
        ASSERT_EQ(lines[static_cast<std::size_t>(i)], std::format("line-{}\n", i));
    }
}

TEST(AsyncLoggerTest, BlocksProducerWhenQueueIsFull) {
    auto gate = SinkGate{};
    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async", .queue_capacity = 1});
    logger.append_sink(BlockingSink{&gate}, "%v");

    logger.info({}, "first");
    ASSERT_TRUE(gate.entered.try_acquire_for(std::chrono::milliseconds(1000)));
    logger.info({}, "second");  // 占满容量为 1 的队列

    std::binary_semaphore third_returned{0};
    auto producer = std::jthread([&logger, &third_returned] {
        logger.info({}, "third");
        third_returned.release();
    });

    EXPECT_FALSE(third_returned.try_acquire_for(std::chrono::milliseconds(100)))
        << "队列已满时生产者没有等待";

    gate.release.release();
    EXPECT_TRUE(third_returned.try_acquire_for(std::chrono::milliseconds(1000)))
        << "工作线程空出槽位后生产者没有被唤醒";

    gate.release.release();
    gate.release.release();
}

TEST(AsyncLoggerTest, OverrunOldestDropsOldestMessageAndReports) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};
    auto entered = std::counting_semaphore<64>{0};
    auto release = std::counting_semaphore<64>{0};

    auto logger = ylog::AsyncLogger(
        ylog::AsyncLoggerConfig{.name = "async",
                                .queue_capacity = 1,
                                .overflow_policy = ylog::OverflowPolicy::OverrunOldest,
                                .err_handler = reports.handler()});
    logger.append_sink(GatedRecordingSink{state, &entered, &release}, "%v");

    logger.info({}, "a");
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));
    logger.info({}, "b");  // 占满容量为 1 的队列
    logger.info({}, "c");  // 覆盖 b

    auto reported = reports.snapshot();
    ASSERT_EQ(reported.size(), 1u);
    EXPECT_NE(reported[0].find("b"), std::string::npos) << reported[0];

    release.release();
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));
    release.release();

    auto lines = AsyncRecordingSink{state}.snapshot();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "a\n");
    EXPECT_EQ(lines[1], "c\n");
}

TEST(AsyncLoggerTest, DiscardNewDropsIncomingMessageAndReports) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};
    auto entered = std::counting_semaphore<64>{0};
    auto release = std::counting_semaphore<64>{0};

    auto logger = ylog::AsyncLogger(
        ylog::AsyncLoggerConfig{.name = "async",
                                .queue_capacity = 1,
                                .overflow_policy = ylog::OverflowPolicy::DiscardNew,
                                .err_handler = reports.handler()});
    logger.append_sink(GatedRecordingSink{state, &entered, &release}, "%v");

    logger.info({}, "a");
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));
    logger.info({}, "b");
    logger.info({}, "c");  // 被丢弃

    auto reported = reports.snapshot();
    ASSERT_EQ(reported.size(), 1u);
    EXPECT_NE(reported[0].find("c"), std::string::npos) << reported[0];

    release.release();
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));
    release.release();

    auto lines = AsyncRecordingSink{state}.snapshot();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "a\n");
    EXPECT_EQ(lines[1], "b\n");
}

TEST(AsyncLoggerTest, NonBlockingPoliciesDoNotWaitForConsumer) {
    for (auto policy : {ylog::OverflowPolicy::OverrunOldest, ylog::OverflowPolicy::DiscardNew}) {
        auto state = std::make_shared<AsyncRecordingSink::State>();
        auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};
        auto entered = std::counting_semaphore<64>{0};
        auto release = std::counting_semaphore<64>{0};

        auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async",
                                                                .queue_capacity = 1,
                                                                .overflow_policy = policy,
                                                                .err_handler = reports.handler()});
        logger.append_sink(GatedRecordingSink{state, &entered, &release}, "%v");

        logger.info({}, "seed");
        ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));

        std::binary_semaphore produced{0};
        auto producer = std::jthread([&logger, &produced] {
            for (int i = 0; i < 50; ++i) {
                logger.info({}, "burst-{}", i);
            }
            produced.release();
        });

        EXPECT_TRUE(produced.try_acquire_for(std::chrono::milliseconds(500)))
            << "非阻塞策略下生产者被消费者拖住了";

        release.release(2);
        producer.join();
    }
}

// flush 用「序号 + 条件变量」而不是入队控制消息：队列满时 flush 仍能立起屏障，
// 因为它不占队列容量。名字按现在验证的行为取。
TEST(AsyncLoggerTest, FlushDoesNotOccupyQueueCapacity) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};
    auto entered = std::counting_semaphore<64>{0};
    auto release = std::counting_semaphore<64>{0};

    auto logger = ylog::AsyncLogger(
        ylog::AsyncLoggerConfig{.name = "async",
                                .queue_capacity = 1,
                                .overflow_policy = ylog::OverflowPolicy::DiscardNew,
                                .err_handler = reports.handler()});
    logger.append_sink(GatedRecordingSink{state, &entered, &release}, "%v");

    logger.info({}, "a");
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));
    logger.info({}, "b");  // 占满队列

    std::binary_semaphore flushed{0};
    auto flusher = std::jthread([&logger, &flushed] {
        logger.flush();
        flushed.release();
    });

    release.release();
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));
    release.release();

    EXPECT_TRUE(flushed.try_acquire_for(std::chrono::milliseconds(2000)))
        << "队列满时 flush 卡住了：屏障不该占用队列容量";
}

TEST(AsyncLoggerTest, BlockPolicyReportsNothing) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};
    auto entered = std::counting_semaphore<64>{0};
    auto release = std::counting_semaphore<64>{0};

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{
        .name = "async", .queue_capacity = 1, .err_handler = reports.handler()});
    logger.append_sink(GatedRecordingSink{state, &entered, &release}, "%v");

    logger.info({}, "a");
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));
    logger.info({}, "b");
    release.release();
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)));
    release.release();

    EXPECT_TRUE(reports.snapshot().empty()) << "Block 策略下不应有丢弃上报";
}

TEST(AsyncLoggerTest, FlushReturnsAfterMessageIsWritten) {
    auto directory = temp_test_path("async-flush");
    std::filesystem::create_directories(directory);
    auto path = directory / "flush.log";

    {
        auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
        logger.append_sink(ylog::FileSink(path.string()), "%v");
        logger.info({}, "persisted");
        logger.flush();

        EXPECT_EQ(read_all(path), "persisted\n");
    }

    std::filesystem::remove_all(directory);
}

TEST(AsyncLoggerTest, FlushIsABarrierForEachProducingThread) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
    logger.append_sink(view, "%v");

    auto pool = std::vector<std::jthread>{};
    for (int tid = 0; tid < 4; ++tid) {
        pool.emplace_back([&logger, &view, tid] {
            auto payload = std::format("tid-{}", tid);
            logger.info({}, "{}", payload);
            logger.flush();

            auto lines = view.snapshot();
            EXPECT_NE(std::find(lines.begin(), lines.end(), payload + "\n"), lines.end())
                << "flush 返回时本条消息尚未落盘";
        });
    }
    for (auto& worker : pool) {
        worker.join();
    }

    EXPECT_EQ(view.snapshot().size(), 4u);
}

TEST(AsyncLoggerTest, StopDrainsQueueAndEveryLaterLogIsReported) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};
    auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};

    auto logger = ylog::AsyncLogger(
        ylog::AsyncLoggerConfig{.name = "async", .err_handler = reports.handler()});
    logger.append_sink(view, "%v");
    for (int i = 0; i < 100; ++i) {
        logger.info({}, "line-{}", i);
    }

    logger.stop();
    EXPECT_EQ(view.snapshot().size(), 100u) << "stop() 没有排空已入队的消息";

    std::binary_semaphore returned{0};
    auto producer = std::jthread([&logger, &returned] {
        logger.info({}, "after-stop-1");
        logger.info({}, "after-stop-2");
        returned.release();
    });

    EXPECT_TRUE(returned.try_acquire_for(std::chrono::milliseconds(200)))
        << "停止后的 log() 阻塞了调用线程";

    auto reported = reports.snapshot();
    ASSERT_EQ(reported.size(), 2u) << "停止后每次入队都应上报，后续丢弃不得静默";
    EXPECT_NE(reported[0].find("after-stop-1"), std::string::npos) << reported[0];
    EXPECT_NE(reported[1].find("after-stop-2"), std::string::npos) << reported[1];
    EXPECT_EQ(view.snapshot().size(), 100u);
}

TEST(AsyncLoggerTest, FlushIsABarrierForEveryCallerUnderConcurrency) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
    logger.append_sink(view, "%v");

    constexpr auto thread_count = 4;
    constexpr auto per_thread = 20;
    auto missing = std::atomic<int>{0};

    auto produce = [&logger, &view, &missing](int tid) {
        for (int i = 0; i < per_thread; ++i) {
            auto line = std::format("t{}-{}", tid, i);
            logger.info({}, "{}", line);
            logger.flush();
            // flush() 返回后，本线程刚写的那条必须已在落盘集合里：
            // 多条并发 flush 合并到同一条控制消息时，也不能被提前结清。
            auto lines = view.snapshot();
            if (std::find(lines.begin(), lines.end(), line + "\n") == lines.end()) {
                ++missing;
            }
        }
    };

    auto pool = std::vector<std::jthread>{};
    for (int tid = 0; tid < thread_count; ++tid) {
        pool.emplace_back(produce, tid);
    }
    for (auto& worker : pool) {
        worker.join();
    }

    EXPECT_EQ(missing.load(), 0) << "有调用方的 flush() 在消息落盘前就返回了";
}

TEST(AsyncLoggerTest, StopRejectsQueuedProducersAndLaterLogs) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};
    auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};

    auto entered = std::counting_semaphore<64>{0};
    auto release = std::counting_semaphore<64>{0};
    auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{
        .name = "async", .queue_capacity = 2, .err_handler = reports.handler()});
    logger.append_sink(GatedRecordingSink{state, &entered, &release}, "%v");

    logger.info({}, "m0");
    ASSERT_TRUE(entered.try_acquire_for(std::chrono::milliseconds(1000)))
        << "worker 没有进入 write";
    logger.info({}, "m1");
    logger.info({}, "m2");

    // 队列已满且 worker 被钉住：这个生产者会停在 Block 的容量等待上。
    auto blocked = std::binary_semaphore{0};
    auto blocked_producer = std::jthread([&logger, &blocked] {
        logger.info({}, "m3");
        blocked.release();
    });

    auto stopper = std::jthread([&logger] { logger.stop(); });

    // 停止请求必须立刻让卡在容量等待上的生产者放弃入队，而不是等队列腾出空位。
    EXPECT_TRUE(blocked.try_acquire_for(std::chrono::seconds(2)))
        << "停止请求之后，卡在 Block 上的生产者仍在等待容量";

    // 闸门对每次 write 都生效，多放几张通行证让排空能走完（多余许可留在计数器里）。
    for (int i = 0; i < 8; ++i) {
        release.release();
    }

    logger.info({}, "late");

    stopper.join();
    blocked_producer.join();

    auto reported = reports.snapshot();
    ASSERT_EQ(reported.size(), 2u) << "停止之后每次拒绝都应上报";
    EXPECT_NE(reported[0].find("m3"), std::string::npos) << reported[0];
    EXPECT_NE(reported[1].find("late"), std::string::npos) << reported[1];
    EXPECT_EQ(view.snapshot().size(), 3u) << "停止请求之后仍有多余消息落盘";
}

TEST(AsyncLoggerTest, FlushAfterStopStillFlushesSinks) {
    auto directory = temp_test_path("async-flush-after-stop");
    std::filesystem::create_directories(directory);
    auto path = directory / "stopped.log";

    {
        auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
        logger.append_sink(ylog::FileSink(path.string()), "%v");
        logger.info({}, "kept");
        logger.stop();

        std::binary_semaphore returned{0};
        auto flusher = std::jthread([&logger, &returned] {
            logger.flush();
            returned.release();
        });
        EXPECT_TRUE(returned.try_acquire_for(std::chrono::milliseconds(500)))
            << "停止后的 flush() 没有立即返回";
        EXPECT_EQ(read_all(path), "kept\n");
    }

    std::filesystem::remove_all(directory);
}

TEST(AsyncLoggerTest, ConcurrentFlushAfterStopIsSafeAndComplete) {
    auto directory = temp_test_path("async-flush-after-stop-concurrent");
    std::filesystem::create_directories(directory);
    auto path = directory / "stopped.log";

    {
        auto logger = ylog::AsyncLogger(ylog::AsyncLoggerConfig{.name = "async"});
        logger.append_sink(ylog::FileSink(path.string()), "%v");
        for (int i = 0; i < 100; ++i) {
            logger.info({}, "line-{}", i);
        }
        logger.stop();

        // 停止后没有 worker 可排队，多个调用线程会各自 flush sink；AnySink 的
        // output_mutex_ 保证同一 sink 串行。这里断言这条路径既不死锁也不丢内容。
        auto pool = std::vector<std::jthread>{};
        for (int tid = 0; tid < 4; ++tid) {
            pool.emplace_back([&logger] {
                for (int i = 0; i < 20; ++i) {
                    logger.flush();
                }
            });
        }
        for (auto& worker : pool) {
            worker.join();
        }
    }

    auto content = read_all(path);
    auto lines = std::count(content.begin(), content.end(), '\n');
    EXPECT_EQ(lines, static_cast<std::ptrdiff_t>(100)) << "停止后的并发 flush 丢行";

    std::filesystem::remove_all(directory);
}

TEST(AsyncLoggerTest, SinkFailureIsReportedAndOtherSinksKeepWorking) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};
    auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};

    auto logger = ylog::AsyncLogger(
        ylog::AsyncLoggerConfig{.name = "async", .err_handler = reports.handler()});
    logger.append_sink(ThrowingSink{}, "%v");
    logger.append_sink(view, "%v");

    logger.info({}, "a");
    logger.info({}, "b");

    ASSERT_TRUE(view.wait_for(2, std::chrono::milliseconds(1000)))
        << "第一个 sink 抛异常后工作线程没有继续投递";

    auto reported = reports.snapshot();
    ASSERT_EQ(reported.size(), 1u) << "连续失败只应在进入失败状态时上报一次";
    EXPECT_NE(reported[0].find("boom"), std::string::npos) << reported[0];
    EXPECT_EQ(view.snapshot().size(), 2u);
}

TEST(AsyncLoggerTest, SinkFailureReportsAgainAfterSuccessfulWrite) {
    struct FlakySink {
        std::shared_ptr<int> calls = std::make_shared<int>(0);

        auto write(std::string_view) -> void {
            if (++*calls % 2 == 1) {
                throw std::runtime_error("boom");
            }
        }
        auto flush() -> void {}
    };

    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};
    auto reports = ErrorReports{std::make_shared<ErrorReports::State>()};

    auto logger = ylog::AsyncLogger(
        ylog::AsyncLoggerConfig{.name = "async", .err_handler = reports.handler()});
    logger.append_sink(FlakySink{}, "%v");
    logger.append_sink(view, "%v");

    // 失败 → 成功 → 失败：成功那次应解除「已上报」状态，于是第二段失败再报一次。
    logger.info({}, "a");
    logger.info({}, "b");
    logger.info({}, "c");

    ASSERT_TRUE(view.wait_for(3, std::chrono::milliseconds(1000)));

    auto reported = reports.snapshot();
    ASSERT_EQ(reported.size(), 2u) << "两段失败应各上报一次，中间的成功要复位状态";
    EXPECT_NE(reported[0].find("boom"), std::string::npos) << reported[0];
    EXPECT_EQ(view.snapshot().size(), 3u);
}

TEST(AsyncLoggerTest, HandlerExceptionDoesNotKillWorker) {
    struct ThrowingHandler {
        auto operator()(std::string_view) const -> void { throw std::runtime_error("handler"); }
    };

    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto logger = ylog::AsyncLogger(
        ylog::AsyncLoggerConfig{.name = "async", .err_handler = ThrowingHandler{}});
    logger.append_sink(ThrowingSink{}, "%v");
    logger.append_sink(view, "%v");

    logger.info({}, "a");
    logger.info({}, "b");

    EXPECT_TRUE(view.wait_for(2, std::chrono::milliseconds(1000)))
        << "err_handler 抛异常后工作线程停止了";
}

TEST(AsyncLoggerTest, DefaultHandlerIsRateLimited) {
    auto config = ylog::AsyncLoggerConfig{.name = "async"};
    auto captured = std::ostringstream{};
    auto* original = std::cerr.rdbuf(captured.rdbuf());

    for (int i = 0; i < 5; ++i) {
        config.err_handler("同一秒内的重复上报");
    }
    std::cerr.flush();
    std::cerr.rdbuf(original);

    auto text = captured.str();
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 1) << "默认 handler 没有限频，输出：\n"
                                                             << text;
    EXPECT_NE(text.find("同一秒内的重复上报"), std::string::npos);
}

TEST(RegistryAsyncTest, CreatesAndFindsAsyncLogger) {
    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create_async("async-registry", ylog::AsyncLoggerConfig{});

    EXPECT_EQ(logger.name(), "async-registry");
    EXPECT_EQ(registry.get_async("async-registry"), &logger);
    EXPECT_EQ(registry.get("async-registry"), nullptr);
    EXPECT_EQ(registry.get_async("missing"), nullptr);
    EXPECT_TRUE(registry.contains("async-registry"));

    registry.drop_all();
    EXPECT_FALSE(registry.contains("async-registry"));
}

TEST(RegistryAsyncTest, RejectsNameCollisionAcrossTypes) {
    auto& registry = ylog::Registry::instance();
    registry.create("collision-sync");

    EXPECT_THROW(registry.create_async("collision-sync", ylog::AsyncLoggerConfig{}),
                 std::invalid_argument);

    registry.create_async("collision-async", ylog::AsyncLoggerConfig{});
    EXPECT_THROW(registry.create("collision-async"), std::invalid_argument);

    registry.drop_all();
}

TEST(RegistryAsyncTest, SetLevelByNameFiltersAsyncMessages) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create_async("async-level", ylog::AsyncLoggerConfig{});
    logger.append_sink(view, "%v");

    EXPECT_TRUE(registry.set_level("async-level", ylog::Level::Warning));
    EXPECT_FALSE(registry.set_level("missing", ylog::Level::Warning));
    EXPECT_EQ(logger.level(), ylog::Level::Warning);

    logger.info({}, "dropped");
    logger.warning({}, "kept");

    ASSERT_TRUE(view.wait_for(1, std::chrono::milliseconds(1000)));

    auto lines = view.snapshot();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "kept\n");

    registry.drop_all();
}

TEST(RegistryAsyncTest, FlushAllIsBarrierForAsyncLoggers) {
    auto directory = temp_test_path("registry-async-flush");
    std::filesystem::create_directories(directory);
    auto path = directory / "flush.log";

    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create_async("async-flush-all", ylog::AsyncLoggerConfig{});
    logger.append_sink(ylog::FileSink(path.string()), "%v");
    logger.info({}, "persisted");

    registry.flush_all();
    EXPECT_EQ(read_all(path), "persisted\n");

    registry.drop_all();
    std::filesystem::remove_all(directory);
}

TEST(RegistryAsyncTest, DropDrainsWithoutExplicitStop) {
    auto state = std::make_shared<AsyncRecordingSink::State>();
    auto view = AsyncRecordingSink{state};

    auto& registry = ylog::Registry::instance();
    auto& logger = registry.create_async("async-drop", ylog::AsyncLoggerConfig{});
    logger.append_sink(view, "%v");
    for (int i = 0; i < 100; ++i) {
        logger.info({}, "line-{}", i);
    }

    EXPECT_TRUE(registry.drop("async-drop"));
    EXPECT_FALSE(registry.contains("async-drop"));

    EXPECT_EQ(view.snapshot().size(), 100u) << "drop() 时没有排空已入队的消息";
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
