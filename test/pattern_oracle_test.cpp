// pattern flag 全集的 oracle 测试：同一 timestamp、同一 logger/level/payload/来源位置，
// 分别用 ylog::PatternFormatter 与 spdlog 1.17.0 的 pattern_formatter 格式化，逐字节比较。
// 事实依据：research 分支的 research/pattern-flags.md（53 条，一手来源 spdlog 1.17.0）。
#include <gtest/gtest.h>

#include <spdlog/details/log_msg.h>
#include <spdlog/details/os.h>
#include <spdlog/pattern_formatter.h>

import std;
import ylog;

namespace {

// 固定时间点：不依赖当前时间，两个实现都用 localtime 解释同一 timestamp。
// 2026-11-18 10:20:30.123（UTC 基准）。
constexpr auto fixed_epoch_seconds = std::time_t{1'795'000'230};

auto fixed_time() -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::from_time_t(fixed_epoch_seconds) +
           std::chrono::milliseconds{123};
}

constexpr auto logger_name = "oracle";
constexpr auto payload = "payload";
constexpr auto source_file = "/tmp/dir/unit_oracle.cpp";
constexpr auto source_function = "oracle_func";
constexpr auto source_line = 42;

auto ylog_message(std::chrono::system_clock::time_point time, bool with_source) -> ylog::Message {
    auto location = with_source ? ylog::SourceLocation{source_file, source_line, source_function}
                                : ylog::SourceLocation{"", 0, ""};
    return ylog::Message(ylog::Level::Info, logger_name, location, payload, time);
}

auto oracle_message(std::chrono::system_clock::time_point time, bool with_source)
    -> spdlog::details::log_msg {
    auto location = with_source ? spdlog::source_loc{source_file, source_line, source_function}
                                : spdlog::source_loc{};
    return spdlog::details::log_msg(time, location, logger_name, spdlog::level::info, payload);
}

auto ylog_output(std::string_view pattern,
                 std::chrono::system_clock::time_point time,
                 bool with_source) -> std::string {
    auto formatter = ylog::PatternFormatter(pattern);
    auto message = ylog_message(time, with_source);
    std::string dest;
    formatter.format(message, dest);
    return dest;
}

auto oracle_output(std::string_view pattern,
                   std::chrono::system_clock::time_point time,
                   bool with_source) -> std::string {
    auto formatter = spdlog::pattern_formatter(std::string(pattern));
    auto message = oracle_message(time, with_source);
    spdlog::memory_buf_t buffer;
    formatter.format(message, buffer);
    return std::string(buffer.data(), buffer.size());
}

auto expect_same(std::string_view pattern,
                 std::chrono::system_clock::time_point time,
                 bool with_source = true) -> void {
    EXPECT_EQ(ylog_output(pattern, time, with_source), oracle_output(pattern, time, with_source))
        << "pattern: " << std::string(pattern);
}

auto expect_same(std::string_view pattern, bool with_source = true) -> void {
    expect_same(pattern, fixed_time(), with_source);
}

// 找一个日号小于 10 的时间点，用于验证 %c 的固定字段宽度（24）与日不补零。
auto time_with_single_digit_day() -> std::chrono::system_clock::time_point {
    auto time = fixed_time();
    for (int step = 0; step < 31; ++step) {
        auto seconds = std::chrono::system_clock::to_time_t(time);
        auto local = std::tm{};
        if (auto* value = std::localtime(&seconds); value != nullptr) {
            local = *value;
        }
        if (local.tm_mday < 10) {
            return time;
        }
        time += std::chrono::hours{24};
    }
    return time;
}

}  // namespace

// 事实 3：%v 输出消息正文；%n 输出 logger 名。
TEST(PatternOracleTest, PayloadAndName) {
    expect_same("%v");
    expect_same("%n");
    expect_same("[%n] %v%n");
    expect_same("%v%v");
}

// 事实 2：%L 单字符短 level 与 spdlog 一致；%l 的完整名沿用 ylog 名称表（大小写不同）。
TEST(PatternOracleTest, LevelFlags) {
    expect_same("%L");
    expect_same("[%L]");

    EXPECT_EQ(ylog_output("%l", fixed_time(), true), "Info\n");
    EXPECT_EQ(ylog_output("[%l]", fixed_time(), true), "[Info]\n");
    EXPECT_EQ(ylog_output("%-8l|", fixed_time(), true), "Info    |\n");
}

// 事实 4：%t 线程 id（Linux 取内核 TID）、%P 每次格式化读取 PID。
TEST(PatternOracleTest, ThreadAndProcessIds) {
    expect_same("%t");
    expect_same("%P");
    expect_same("[%t] %P");

    auto message = ylog_message(fixed_time(), true);
    EXPECT_EQ(ylog_output("%t", fixed_time(), true), std::format("{}\n", message.thread_id()));
    EXPECT_GT(message.thread_id(), 0u);

    // 线程 id 在 Message 构造点捕获：工作线程里构造的消息报告工作线程，而不是格式化线程。
    std::size_t ylog_thread = 0;
    std::size_t oracle_thread = 0;
    std::jthread worker([&] {
        ylog_thread = ylog_message(fixed_time(), true).thread_id();
        oracle_thread = spdlog::details::os::thread_id();
    });
    worker.join();
    EXPECT_GT(ylog_thread, 0u);
    EXPECT_EQ(ylog_thread, oracle_thread);
}

// 事实 5-15：日期时间 flag 全集与 spdlog 逐字节一致。
TEST(PatternOracleTest, CalendarFlags) {
    for (auto flag : {"%a", "%A", "%b", "%h", "%B", "%c", "%C", "%D", "%x", "%m", "%d",
                      "%H", "%I", "%M", "%S", "%p", "%r", "%R", "%T", "%X", "%z"}) {
        expect_same(flag);
    }
    expect_same("%a %A %b %B %c");
    expect_same("%Y-%m-%dT%H:%M:%S%z");
    expect_same("%D %T %R %r %p");

    // 日不补零 + 固定字段宽度（%c 按 24 计算 padding，事实 6/23）。
    auto single_digit_day = time_with_single_digit_day();
    expect_same("%c", single_digit_day);
    expect_same("%25c", single_digit_day);
    expect_same("%=25c", single_digit_day);
    expect_same("%-25c", single_digit_day);
}

// 事实 7/12：%Y 四位年、%e/%f/%F 亚秒、%E epoch 秒。
TEST(PatternOracleTest, SubsecondAndEpochFlags) {
    for (auto flag : {"%Y", "%C", "%e", "%f", "%F", "%E"}) {
        expect_same(flag);
    }
    expect_same("%H:%M:%S.%e");
    expect_same("%H:%M:%S.%f");
    expect_same("%H:%M:%S.%F");
    expect_same("%Y-%m-%d %H:%M:%S.%e");
}

// 事实 17/18：来源位置 flag；无来源时只走 padding 不输出。
TEST(PatternOracleTest, SourceFlags) {
    for (auto flag : {"%@", "%g", "%s", "%#", "%!", "[%s:%#] %!", "%s %! %#", "%10!", "%3!!"}) {
        expect_same(flag);
    }
    for (auto flag : {"%@", "%g", "%s", "%#", "%!", "[%s:%#]", "%-10s|", "%10s|"}) {
        expect_same(flag, false);
    }
    EXPECT_EQ(ylog_output("%@", fixed_time(), true), "/tmp/dir/unit_oracle.cpp:42\n");
    EXPECT_EQ(ylog_output("%s", fixed_time(), true), "unit_oracle.cpp\n");
}

// 事实 23-27：padding 对齐、上限 64、`!` 截断。
TEST(PatternOracleTest, Padding) {
    for (auto pattern : {"%8v", "%-8v", "%=8v", "%08v", "%1v", "%0v", "%100v", "%-64v", "%8!v",
                         "%-8!v", "%=8!v", "%1!v", "%2!n", "%3!s", "%2!@"}) {
        expect_same(pattern);
    }
    expect_same("[%-8n]%=10v|");

    // 补位字符只有空格：%08l 不是零填充（事实 27）。
    EXPECT_EQ(ylog_output("%08l", fixed_time(), true), "    Info\n");
}

// 事实 22/28：未知 flag 原样输出、%% 与末尾 %、畸形 pattern 不报错。
TEST(PatternOracleTest, UnknownAndMalformedPatterns) {
    for (auto pattern : {"%q", "%Q", "100%%", "abc%", "%", "%.3v", "%-v", "%=v", "%10q", "%!q"}) {
        expect_same(pattern);
    }
    EXPECT_EQ(ylog_output("%q", fixed_time(), true), "%q\n");
    EXPECT_EQ(ylog_output("100%%", fixed_time(), true), "100%\n");
    EXPECT_EQ(ylog_output("abc%", fixed_time(), true), "abc\n");
    EXPECT_EQ(ylog_output("%.3v", fixed_time(), true), "%.3v\n");
}

// 事实 16/1：%^/%$ 只记录色彩区间（不写转义序列）；%+ 在 level 名上标出该区间。
TEST(PatternOracleTest, ColorRange) {
    auto formatter = ylog::PatternFormatter("a%^bc%$d");
    auto message = ylog_message(fixed_time(), true);
    std::string dest;
    formatter.format(message, dest);
    EXPECT_EQ(dest, "abcd\n");
    EXPECT_EQ(formatter.color_range(), (std::pair<std::size_t, std::size_t>{1, 3}));

    auto oracle_formatter = spdlog::pattern_formatter("a%^bc%$d");
    auto oracle_msg = oracle_message(fixed_time(), true);
    spdlog::memory_buf_t buffer;
    oracle_formatter.format(oracle_msg, buffer);
    EXPECT_EQ(std::string(buffer.data(), buffer.size()), "abcd\n");
    EXPECT_EQ(oracle_msg.color_range_start, 1u);
    EXPECT_EQ(oracle_msg.color_range_end, 3u);
}

// 事实 20：%u/%i/%o/%O 为距上一条消息的纳秒/微秒/毫秒/秒数，负差值按 0 处理。
TEST(PatternOracleTest, ElapsedFlags) {
    auto first = fixed_time();
    auto second = first + std::chrono::milliseconds{1500};
    auto backward = second - std::chrono::seconds{1};

    auto formatter = ylog::PatternFormatter("%O|%o|%i|%u");
    auto oracle_formatter = spdlog::pattern_formatter("%O|%o|%i|%u");

    std::string first_out, second_out, backward_out;
    formatter.format(ylog_message(first, true), first_out);
    formatter.format(ylog_message(second, true), second_out);
    formatter.format(ylog_message(backward, true), backward_out);

    spdlog::memory_buf_t first_buf, second_buf, backward_buf;
    oracle_formatter.format(oracle_message(first, true), first_buf);
    oracle_formatter.format(oracle_message(second, true), second_buf);
    oracle_formatter.format(oracle_message(backward, true), backward_buf);

    EXPECT_EQ(second_out, std::string(second_buf.data(), second_buf.size()));
    EXPECT_EQ(second_out, "1|1500|1500000|1500000000\n");
    EXPECT_EQ(backward_out, std::string(backward_buf.data(), backward_buf.size()));
    EXPECT_EQ(backward_out, "0|0|0|0\n");
}

// 事实 1/38：%+ 的段落结构与字段；无来源时省略 [file:line]；忽略 padding。
TEST(PatternOracleTest, FullFlag) {
    auto date = oracle_output("[%Y-%m-%d %H:%M:%S.%e]", fixed_time(), true);
    date.pop_back();  // 去掉 eol
    auto level = std::string(ylog::level_name(ylog::Level::Info));

    auto expected = std::format("{} [{}] [{}] [unit_oracle.cpp:{}] {}\n", date, logger_name, level,
                                source_line, payload);
    EXPECT_EQ(ylog_output("%+", fixed_time(), true), expected);

    auto expected_no_source = std::format("{} [{}] [{}] {}\n", date, logger_name, level, payload);
    EXPECT_EQ(ylog_output("%+", fixed_time(), false), expected_no_source);

    EXPECT_EQ(ylog_output("%10+", fixed_time(), true), ylog_output("%+", fixed_time(), true));
    EXPECT_EQ(oracle_output("%10+", fixed_time(), true), oracle_output("%+", fixed_time(), true));
}

// 事实 21：%& 输出 MDC；ylog 暂无 MDC 容器，与 spdlog 空 MDC 一致输出为空。
TEST(PatternOracleTest, MdcFlagIsEmpty) {
    expect_same("%&");
    expect_same("[%&] %v");
    EXPECT_EQ(ylog_output("[%&] %v", fixed_time(), true), "[] payload\n");
}

// 事实全集组合：常用 pattern 与 spdlog 完全一致（%l 名称大小写除外）。
TEST(PatternOracleTest, CombinedPatterns) {
    expect_same("%Y-%m-%d %H:%M:%S.%e [%L] [%n] [%t] [%s:%#] %v");
    expect_same("%L %t %P %s:%# %! %v");
    expect_same("%a %b %d %T %Y %z [%L] %v");
    expect_same("%e|%f|%F|%E");
}
