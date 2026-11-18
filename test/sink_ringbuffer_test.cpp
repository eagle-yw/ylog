#include "gtest/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

import std;
import ylog;
import test_support;

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
