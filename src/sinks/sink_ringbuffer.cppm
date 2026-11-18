export module ylog:sink.ringbuffer;

import std;
import :sink;
import ylog_detail_errors;

namespace ylog {

// 环形缓冲的读句柄，与被写的 sink 分开：AnySink 不暴露底层 sink，读者因此拿到的是
// 这个独立的 store（ADR-0009）。写入方是 logger 的投递线程，读取方可以在任意线程，
// 所以它自带互斥量——这是 sink 家族里唯一的锁。
export class RingBufferStore {
public:
    // 容量在构造期固定：运行期改容量会让「最近 n 条」的语义随时漂移。
    explicit RingBufferStore(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                "RingBufferStore: capacity must be greater than 0");
        }
    }

    auto push(std::string_view line) -> void {
        std::lock_guard lock(mutex_);
        lines_.emplace_back(line);
        while (lines_.size() > capacity_) {
            lines_.pop_front();
        }
    }

    // 最近 count 条，按时间正序；count >= size() 时返回全部。
    auto last(std::size_t count) const -> std::vector<std::string> {
        std::lock_guard lock(mutex_);
        auto first = lines_.size() > count ? lines_.size() - count : std::size_t{0};
        auto result = std::vector<std::string>{};
        result.reserve(lines_.size() - first);
        result.insert(result.end(), std::next(lines_.begin(), static_cast<std::ptrdiff_t>(first)),
                      lines_.end());
        return result;
    }

    auto size() const -> std::size_t {
        std::lock_guard lock(mutex_);
        return lines_.size();
    }

    auto capacity() const -> std::size_t { return capacity_; }

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
};

// 把行写进调用方持有的 store。store 必须比使用它的 logger 活得久（sink 只存指针）。
// 没有 I/O，因此不配 err_handler。
export class RingBufferSink {
public:
    explicit RingBufferSink(RingBufferStore& store)
        : store_(&store) {}

    auto write(std::string_view line) -> void { store_->push(line); }

private:
    RingBufferStore* store_;
};

}  // namespace ylog
