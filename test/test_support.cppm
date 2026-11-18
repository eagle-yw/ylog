export module test_support;

import std;
import ylog;

// 测试共用夹具：从 unit_test.cpp 提出来，供各套件的 TU 复用。
// 内部链接的名字无法导出，所以这里全部具名导出，由 test_support 这个模块兜住。

export struct Point {
    int x;
    double y;
};

// 把 worker 钉在一次 write 上的闸门：write 先放行 entered，再等 release。
export struct SinkGate {
    std::counting_semaphore<64> entered{0};
    std::counting_semaphore<64> release{0};
};

export struct BlockingSink {
    SinkGate* gate = nullptr;

    auto write(std::string_view) -> void {
        gate->entered.release();
        gate->release.acquire();
    }
    auto flush() -> void {}
};

export struct ThrowingSink {
    auto write(std::string_view) -> void { throw std::runtime_error("boom"); }
    auto flush() -> void {}
};

// 测试自己造的行：生产者号-序号-固定载荷。按字段解析而不是按下标切字符串，
// 断言失败时才说得清是哪个字段坏了。
export struct ProbeLine {
    int producer = 0;
    int sequence = 0;
    std::string payload;
};

export auto parse_probe_line(std::string_view line) -> std::optional<ProbeLine> {
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
export auto temp_test_path(std::string_view name) -> std::filesystem::path {
    static std::atomic<std::uint64_t> counter{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           std::format("ylog-rotating-{}-{}-{}", name, stamp, counter.fetch_add(1));
}

export auto read_all(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// 一点字节都不接受的底层：让任何写入都以 badbit 失败，用来验证热路径的失败上报。
export struct FailingStreambuf : std::streambuf {
    auto xsputn(const char*, std::streamsize) -> std::streamsize override { return 0; }
    auto overflow(int_type) -> int_type override { return traits_type::eof(); }
};

// 只数 flush 次数的 sink：flush_on 的验收要回答「是谁触发了 flush」。
export struct FlushCountingSink {
    std::atomic<int>* flushes;
    auto write(std::string_view) -> void {}
    auto flush() -> void { flushes->fetch_add(1, std::memory_order_relaxed); }
};

// flush 卡在闸门上的 sink：worker 停在 flush 里时，生产者必须已经返回。
export struct GatedFlushSink {
    std::atomic<bool>* entered;
    std::counting_semaphore<1>* release;

    auto write(std::string_view) -> void {}
    auto flush() -> void {
        *entered = true;
        release->acquire();
    }
};

// 收集行的 sink：断言「收到了什么字节」的用例共用它。
export struct CaptureSink {
    std::string* out;
    auto write(std::string_view line) -> void { out->append(line); }
};

// 声明自己接受颜色的捕获 sink：用于不经过具体 sink 的上色用例。
export struct ColoringCaptureSink {
    std::string* out;
    ylog::ColorMode mode;

    auto write(std::string_view line) -> void { out->append(line); }
    auto color_mode() const -> ylog::ColorMode { return mode; }
};

// 轮询等待，避免用固定 sleep 猜 worker 的进度。
export auto wait_until(const std::function<bool()>& predicate,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
    -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// 一条经过 OstreamSink 的完整输出：用来断言「哪些字节该出现、哪些不该」。
export auto ostream_sink_output(ylog::Level level, ylog::ColorMode mode, std::string_view pattern)
    -> std::string {
    auto captured = std::ostringstream{};
    auto sink = ylog::OstreamSink(ylog::OstreamConfig{.stream = &captured, .color = mode});
    auto any = ylog::AnySink(std::move(sink), std::make_unique<ylog::PatternFormatter>(pattern));
    any.write(ylog::Message(level, "n", {}, "msg"));
    return captured.str();
}

export auto local_time_point(int year, int month, int day, int hour, int minute, int second)
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

// 异步组件的观察点：sink 侧记录行内容并通知，测试据此等待，不 sleep 轮询。
export struct AsyncRecordingSink {
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
export struct ErrorReports {
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
export struct GatedRecordingSink {
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
