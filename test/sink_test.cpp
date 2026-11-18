#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

TEST(SinkTest, FormatterSeamRejectsEmptyFormatter) {
    auto captured = std::string{};
    auto any =
        ylog::AnySink(CaptureSink{&captured}, std::make_unique<ylog::PatternFormatter>("%v"));

    EXPECT_THROW(any.set_formatter(nullptr), std::invalid_argument);

    // 拒绝之后原 formatter 仍在：这次违约不改变 sink 的可用状态。
    any.write(ylog::Message(ylog::Level::Info, "n", {}, "still"));
    EXPECT_EQ(captured, "still\n");
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
