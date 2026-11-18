export module ylog:registry;

import std;
import :level;
import :logger;
import :async;
import :sink;
import :sink.stdout;
import ylog_detail_errors;

namespace ylog {

// 全局 logger 注册表。同步 Logger 与异步 AsyncLogger 共用一个命名空间。
//
// 命名规则（设计记录见 docs/adr/0007-registry-namespace-and-ownership.md）：
// - 一个名字只能属于一个组件；
// - create() 对同名同步 logger 幂等，撞上异步组件抛 std::invalid_argument；
// - create_async() 撞上任何已用名字都抛 std::invalid_argument；
// - get()/get_async() 按类型查找，名字属于另一类时返回 nullptr。
//
// 所有权：Registry 是唯一持有者。drop()/drop_all() 先摘除、再在锁外析构，所以异步
// 组件的排空发生在 drop() 返回之前——不存在「最后一个外部持有者析构才停机」的模型。
//
// 并发契约：创建/摘除/配置属配置期操作；flush_all() 先取快照再逐个 flush，避免持锁
// 等待异步组件的屏障。
export class Registry {
public:
    static auto instance() -> Registry& {
        static Registry registry;
        return registry;
    }

    // 创建并注册同步 logger。同名同步 logger 已存在则返回既有实例（幂等）。
    auto create(std::string name) -> Logger& {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(name);
        if (it == entries_.end()) {
            it = entries_.try_emplace(name).first;
        } else if (it->second.asynchronous) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                std::format("logger 名称已被异步组件占用: {}", name));
        }
        auto& entry = it->second;
        if (!entry.synchronous) {
            entry.synchronous = std::make_unique<Logger>(name);
        }
        return *entry.synchronous;
    }

    // 创建异步组件。名称已被任何组件占用时抛 std::invalid_argument。
    auto create_async(std::string name, AsyncLoggerConfig config) -> AsyncLogger& {
        std::lock_guard lock(mutex_);
        if (auto it = entries_.find(name); it != entries_.end() && it->second.occupied()) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                std::format("logger 名称已被占用: {}", name));
        }
        config.name = name;
        // 先构造再插入：构造失败时不留下空槽位。
        auto created = std::make_unique<AsyncLogger>(std::move(config));
        auto& reference = *created;
        entries_.try_emplace(name).first->second.asynchronous = std::move(created);
        return reference;
    }

    // 按名称查找同步 logger，不存在或名字属于异步组件时返回 nullptr。
    auto get(std::string_view name) -> Logger* {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(std::string(name));
        return it != entries_.end() ? it->second.synchronous.get() : nullptr;
    }

    // 按名称查找异步组件，不存在或名字属于同步 logger 时返回 nullptr。
    auto get_async(std::string_view name) -> AsyncLogger* {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(std::string(name));
        return it != entries_.end() ? it->second.asynchronous.get() : nullptr;
    }

    auto contains(std::string_view name) -> bool {
        std::lock_guard lock(mutex_);
        return entries_.contains(std::string(name));
    }

    // 摘除并销毁：同步 logger 直接释放，异步组件在锁外排空后退出。
    auto drop(std::string_view name) -> bool {
        auto removed = Entry{};
        {
            std::lock_guard lock(mutex_);
            auto it = entries_.find(std::string(name));
            if (it == entries_.end()) {
                return false;
            }
            removed = std::move(it->second);
            entries_.erase(it);
        }
        return true;  // removed 在此析构，异步组件的排空不持注册表锁
    }

    auto drop_all() -> void {
        auto entries = Table{};
        {
            std::lock_guard lock(mutex_);
            entries.swap(entries_);
        }
        // 局部表在此析构：异步组件逐个排空，期间不持注册表锁。
    }

    auto set_level(Level level) -> void {
        std::lock_guard lock(mutex_);
        for (auto& [name, entry] : entries_) {
            entry.apply_level(level);
        }
    }

    // 按名称设置级别，作用于同步或异步组件；名称不存在返回 false。
    auto set_level(std::string_view name, Level level) -> bool {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(std::string(name));
        if (it == entries_.end()) {
            return false;
        }
        it->second.apply_level(level);
        return true;
    }

    auto flush_all() -> void {
        auto loggers = std::vector<Logger*>{};
        auto async_loggers = std::vector<AsyncLogger*>{};
        {
            std::lock_guard lock(mutex_);
            loggers.reserve(entries_.size());
            async_loggers.reserve(entries_.size());
            for (auto& [name, entry] : entries_) {
                if (entry.synchronous) {
                    loggers.push_back(entry.synchronous.get());
                } else {
                    async_loggers.push_back(entry.asynchronous.get());
                }
            }
        }
        for (auto* logger : loggers) {
            logger->flush();
        }
        for (auto* logger : async_loggers) {
            logger->flush();
        }
    }

    Registry(const Registry&) = delete;
    auto operator=(const Registry&) -> Registry& = delete;

private:
    // 一个名字对应一个条目，二者恰有一个非空。
    struct Entry {
        std::unique_ptr<Logger> synchronous;
        std::unique_ptr<AsyncLogger> asynchronous;

        auto occupied() const -> bool { return synchronous || asynchronous; }

        auto apply_level(Level level) -> void {
            if (synchronous) {
                synchronous->set_level(level);
            } else if (asynchronous) {
                asynchronous->set_level(level);
            }
        }
    };

    using Table = std::unordered_map<std::string, Entry>;

    Registry() = default;
    Table entries_;
    mutable std::mutex mutex_;
};

// 便捷自由函数
export auto create_logger(std::string name) -> Logger& {
    return Registry::instance().create(std::move(name));
}

export auto get_logger(std::string_view name) -> Logger* { return Registry::instance().get(name); }

}  // namespace ylog
