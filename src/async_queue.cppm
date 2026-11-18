module;

// assert 是宏，`import std;` 带不进来；内部不变量要用它，只能在全局模块片段里取。
#include <cassert>

// 队列与 Flush 屏障的内部 owner。实现分区、且不导出任何名字：同一模块的 :async
// 能用它，模块外既看不见也命名不了（同 :logger.base）。公开面因此只有 AsyncLogger。
module ylog:async.queue;

import std;
import :message;

namespace ylog {
namespace detail {

// 队列溢出策略的内部拼写。公开的 OverflowPolicy 是配置面上的拼写，留在 :async；
// 两者的对应关系只出现在 AsyncLogger 构造前的那一对映射函数里。
enum class admission_policy {
    block,
    overrun_oldest,
    discard_new,
};

// 一次入队的后果。丢失条目的文本在消息还在栈上时拷出，上报因此可以留在锁外：
// 覆盖是被覆盖的那条，丢弃与拒绝是来件本身。入队成功时为空。
struct admission {
    enum class outcome {
        enqueued,
        overran,
        discarded,
        rejected,
    };

    outcome result = outcome::enqueued;
    std::string lost;
};

// 工作线程的下一步：唯一判定「该投递、该落盘、还是该退出」的地方。
// deliver 带出消息与它的序号，flush 带出待结算的目标；两种后续动作都在锁外进行，
// 完成后分别用 note_delivered / note_flushed 回报。
struct work {
    enum class kind {
        deliver,
        flush,
        finished,
    };

    kind next = kind::finished;
    std::optional<Message> message;  // deliver
    std::size_t sequence = 0;        // deliver：该条序号
    std::size_t target = 0;          // flush：待结算的落盘目标
};

// flush() 的入口判定：目标之内已全部落盘、工作线程还在、或工作线程已退出。
struct barrier_request {
    enum class kind {
        satisfied,
        wait,
        worker_gone,
    };

    kind next = kind::satisfied;
    std::size_t target = 0;
};

// 有界队列与 Flush 屏障的 owner。三条转换规则都在这里：入队（谁进来、满了怎么办）、
// 屏障（等什么、什么时候结清）、停止（排空到哪为止）。
//
// 接口的每个出口都把「需要调用方在锁外做的事」交出去：投递消息、落盘、上报。
// 锁内不做 sink I/O，也不调 ErrorHandler（ADR-0003）。见 ADR-0003 的机制追加一节。
class QueueBarrier {
public:
    QueueBarrier(std::size_t capacity, admission_policy policy)
        : capacity_(capacity),
          policy_(policy) {
        assert(capacity_ > 0);
    }

    QueueBarrier(const QueueBarrier&) = delete;
    auto operator=(const QueueBarrier&) -> QueueBarrier& = delete;
    QueueBarrier(QueueBarrier&&) = delete;
    auto operator=(QueueBarrier&&) -> QueueBarrier& = delete;

    ~QueueBarrier() = default;

    auto capacity() const -> std::size_t { return capacity_; }
    auto policy() const -> admission_policy { return policy_; }

    // 生产者的唯一入口。Block 在内部等空位，另外两种当场决定后果。
    // flush_on_arrival：本条达到 Flush 阈值，入队成功后把屏障目标推到它——
    // 生产者不等落盘、不占槽位，与 flush() 共用同一套结算点。
    auto admit(Message message, bool flush_on_arrival) -> admission;

    // 工作线程的唯一判定点。返回 flush 时调用方落盘后须 note_flushed，
    // 返回 deliver 时须投递后 note_delivered，返回 finished 时停止完成已记账并唤醒所有等待者。
    auto next_work(const std::stop_token& token) -> work;
    auto note_delivered(std::size_t sequence) -> void;
    auto note_flushed(std::size_t target) -> void;

    // 屏障：arm 决定这次 flush 等什么，await 等它结清。
    auto arm_barrier() -> barrier_request;
    auto await_barrier(std::size_t target) -> void;

    // 停机：先在锁内置位，把「要排空的集合」固定在此刻之前已入队的消息，
    // 再唤醒两处等待者，让它们走停止分支而不是继续睡。
    auto begin_stop() -> void;

private:
    struct queued_message {
        Message message;
        std::size_t sequence;
    };

    // 调用方持有 mutex_。
    auto push_locked(Message message, bool flush_on_arrival) -> void;
    // 调用方必须持有 mutex_。摘掉最旧的一条（OverrunOldest），返回它的文本供上报。
    auto evict_oldest_locked() -> std::string;

    std::mutex mutex_;
    std::condition_variable_any cv_ready_;
    std::condition_variable_any cv_space_;
    std::condition_variable_any cv_flushed_;
    // 队列里只有消息：序号在入队时分配，屏障用它表达「刷到哪一条为止」。
    std::deque<queued_message> queue_;
    std::size_t capacity_;
    admission_policy policy_;
    std::size_t next_sequence_{0};       // 下一个待分配的序号 = 已入队条数
    std::size_t processed_sequence_{0};  // 已被 worker 写出的条数
    std::size_t flush_target_{0};        // 当前最高的屏障目标（0 表示无请求）
    std::size_t flushed_sequence_{0};    // 已确认落盘的条数
    bool stopping_{false};
    bool worker_finished_{false};
};

inline auto QueueBarrier::admit(Message message, bool flush_on_arrival) -> admission {
    auto result = admission{};
    {
        std::unique_lock lock(mutex_);
        if (stopping_) {
            result.result = admission::outcome::rejected;
            result.lost = std::string(message.formatted());
        } else {
            switch (policy_) {
                case admission_policy::block:
                    // 停止请求也要唤醒等容量的生产者：让它放弃入队，而不是等到
                    // 队列腾出空位——排空期间队列可能永远不空。
                    cv_space_.wait(lock, [this] { return stopping_ || queue_.size() < capacity_; });
                    if (stopping_) {
                        result.result = admission::outcome::rejected;
                        result.lost = std::string(message.formatted());
                    } else {
                        push_locked(std::move(message), flush_on_arrival);
                    }
                    break;
                case admission_policy::overrun_oldest:
                    if (queue_.size() >= capacity_) {
                        result.result = admission::outcome::overran;
                        result.lost = evict_oldest_locked();
                    }
                    push_locked(std::move(message), flush_on_arrival);
                    break;
                case admission_policy::discard_new:
                    if (queue_.size() >= capacity_) {
                        result.result = admission::outcome::discarded;
                        result.lost = std::string(message.formatted());
                    } else {
                        push_locked(std::move(message), flush_on_arrival);
                    }
                    break;
            }
        }
    }
    // 唤醒放在锁外：worker 被叫醒后要立刻拿这把锁，持锁通知只多一次交接。
    if (result.result == admission::outcome::enqueued ||
        result.result == admission::outcome::overran) {
        cv_ready_.notify_one();
    }
    return result;
}

inline auto QueueBarrier::next_work(const std::stop_token& token) -> work {
    for (;;) {
        std::unique_lock lock(mutex_);
        cv_ready_.wait(lock, token,
                       [this] { return !queue_.empty() || flush_target_ > flushed_sequence_; });

        // 屏障按序号结算，而不是绑在「队列排空」上：目标之内的消息都写出后
        // 立刻落盘，flush_on 因此能在那一条之后落盘，flush() 也能更早返回。
        if (flush_target_ > flushed_sequence_ && processed_sequence_ >= flush_target_) {
            auto target = flush_target_;
            lock.unlock();
            return work{.next = work::kind::flush, .message = std::nullopt, .target = target};
        }
        if (!queue_.empty()) {
            auto item = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            // 槽位已空出：Block 的生产者不必等这条写完就能入队。
            cv_space_.notify_one();
            return work{.next = work::kind::deliver,
                        .message = std::move(item.message),
                        .sequence = item.sequence};
        }
        // 走到这里只剩「已收到停止请求」一种可能：屏障目标总是在入队成功后取刚分配的
        // 那个序号，而队列按 FIFO 写、覆盖策略只删队首，所以目标之内的序号要么被写出、
        // 要么被更晚的写出顺带推过，processed_sequence_ 必定追上它。
        assert(flush_target_ <= flushed_sequence_ || token.stop_requested());
        if (!token.stop_requested()) {
            continue;  // 谓词为真却无活可干：回去等下一个转换
        }
        worker_finished_ = true;
        lock.unlock();
        // 唤醒可能仍卡在容量上或屏障上的调用方，让它们走停止分支而不是继续睡。
        cv_space_.notify_all();
        cv_ready_.notify_all();
        cv_flushed_.notify_all();
        return work{.next = work::kind::finished, .message = std::nullopt};
    }
}

inline auto QueueBarrier::note_delivered(std::size_t sequence) -> void {
    std::lock_guard lock(mutex_);
    // 写入在锁外完成之后才推进：提前推进会让 flush() 在消息没写完时返回。
    processed_sequence_ = sequence + 1;
}

inline auto QueueBarrier::note_flushed(std::size_t target) -> void {
    std::lock_guard lock(mutex_);
    flushed_sequence_ = std::max(flushed_sequence_, target);
    cv_flushed_.notify_all();
}

inline auto QueueBarrier::arm_barrier() -> barrier_request {
    std::lock_guard lock(mutex_);
    auto target = next_sequence_;
    if (worker_finished_) {
        // 工作线程已退出，没人会把 sink 推进到目标：调用方自己落盘。
        return barrier_request{.next = barrier_request::kind::worker_gone, .target = target};
    }
    if (flushed_sequence_ >= target) {
        return barrier_request{.next = barrier_request::kind::satisfied, .target = target};
    }
    flush_target_ = std::max(flush_target_, target);
    cv_ready_.notify_one();
    return barrier_request{.next = barrier_request::kind::wait, .target = target};
}

inline auto QueueBarrier::await_barrier(std::size_t target) -> void {
    std::unique_lock lock(mutex_);
    // 谓词而不是计数：arm 与 await 之间目标可能已被结算，此时立即返回。
    cv_flushed_.wait(lock, [this, target] { return flushed_sequence_ >= target; });
}

inline auto QueueBarrier::begin_stop() -> void {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_ready_.notify_all();
    cv_space_.notify_all();
}

inline auto QueueBarrier::push_locked(Message message, bool flush_on_arrival) -> void {
    queue_.push_back(queued_message{std::move(message), next_sequence_++});
    if (flush_on_arrival) {
        // Flush 阈值折算成一次屏障请求：生产者不等落盘，也不占队列槽位。
        flush_target_ = std::max(flush_target_, next_sequence_);
    }
    // 不变量：队列里只有消息，长度受容量约束，flush 不占用队列空间。
    assert(queue_.size() <= capacity_);
}

inline auto QueueBarrier::evict_oldest_locked() -> std::string {
    auto text = std::string(queue_.front().message.formatted());
    queue_.pop_front();
    return text;
}

}  // namespace detail
}  // namespace ylog
