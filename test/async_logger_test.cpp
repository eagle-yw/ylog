#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

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
