export module ylog:registry;

import std;
import :level;
import :logger;
import :async;
import :sink;
import :sink.stdout;
import ylog_detail_errors;

namespace ylog {

// 全局 logger 注册表。同步 Logger 与异步 AsyncLogger 共用同一个命名空间：
// 按名称查找时两者不互相混淆，但重名创建会被拒绝。
// 并发契约：创建/摘除/配置属配置期操作；flush_all 会先取快照再逐个 flush，
// 避免持锁等待异步组件的屏障。
export class Registry {
public:
    static auto instance() -> Registry& {
        static Registry registry;
        return registry;
    }

    // 创建新 logger 并注册。同名同步 logger 已存在则返回既有实例；
    // 名称被异步组件占用时抛 std::invalid_argument。
    auto create(std::string name) -> Logger& {
        std::lock_guard lock(mutex_);
        if (async_loggers_.contains(name)) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                std::format("logger 名称已被异步组件占用: {}", name));
        }
        auto [it, inserted] = loggers_.try_emplace(name, nullptr);
        if (inserted) {
            it->second = std::make_unique<Logger>(name);
        }
        return *it->second;
    }

    // 创建异步组件。名称在任一命名空间已存在时抛 std::invalid_argument。
    auto create_async(std::string name, AsyncLoggerConfig config) -> AsyncLogger& {
        std::lock_guard lock(mutex_);
        if (loggers_.contains(name) || async_loggers_.contains(name)) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                std::format("logger 名称已被占用: {}", name));
        }
        config.name = name;
        // 先构造再插入：构造失败时不留下空槽位。
        auto created = std::make_unique<AsyncLogger>(std::move(config));
        auto& reference = *created;
        async_loggers_.emplace(std::move(name), std::move(created));
        return reference;
    }

    // 按名称查找同步 logger，不存在或名字属于异步组件时返回 nullptr。
    auto get(std::string_view name) -> Logger* {
        std::lock_guard lock(mutex_);
        auto it = loggers_.find(std::string(name));
        return it != loggers_.end() ? it->second.get() : nullptr;
    }

    // 按名称查找异步组件，不存在或名字属于同步 logger 时返回 nullptr。
    auto get_async(std::string_view name) -> AsyncLogger* {
        std::lock_guard lock(mutex_);
        auto it = async_loggers_.find(std::string(name));
        return it != async_loggers_.end() ? it->second.get() : nullptr;
    }

    auto contains(std::string_view name) -> bool {
        std::lock_guard lock(mutex_);
        auto key = std::string(name);
        return loggers_.contains(key) || async_loggers_.contains(key);
    }

    // 只从注册表摘除，不主动停机：析构在锁外发生，异步组件会先排空再退出。
    auto drop(std::string_view name) -> bool {
        auto removed_logger = std::unique_ptr<Logger>{};
        auto removed_async = std::unique_ptr<AsyncLogger>{};
        auto removed = false;
        {
            std::lock_guard lock(mutex_);
            auto key = std::string(name);
            if (auto it = loggers_.find(key); it != loggers_.end()) {
                removed_logger = std::move(it->second);
                loggers_.erase(it);
                removed = true;
            } else if (auto async_it = async_loggers_.find(key); async_it != async_loggers_.end()) {
                removed_async = std::move(async_it->second);
                async_loggers_.erase(async_it);
                removed = true;
            }
        }
        return removed;
    }

    auto drop_all() -> void {
        auto loggers = std::unordered_map<std::string, std::unique_ptr<Logger>>{};
        auto async_loggers = std::unordered_map<std::string, std::unique_ptr<AsyncLogger>>{};
        {
            std::lock_guard lock(mutex_);
            loggers.swap(loggers_);
            async_loggers.swap(async_loggers_);
        }
        // 局部容器在此析构：异步组件逐个排空，期间不持注册表锁。
    }

    auto set_level(Level level) -> void {
        std::lock_guard lock(mutex_);
        for (auto& [name, logger] : loggers_) {
            logger->set_level(level);
        }
        for (auto& [name, logger] : async_loggers_) {
            logger->set_level(level);
        }
    }

    // 按名称设置级别，作用于同步或异步组件；名称不存在返回 false。
    auto set_level(std::string_view name, Level level) -> bool {
        std::lock_guard lock(mutex_);
        auto key = std::string(name);
        if (auto it = loggers_.find(key); it != loggers_.end()) {
            it->second->set_level(level);
            return true;
        }
        if (auto async_it = async_loggers_.find(key); async_it != async_loggers_.end()) {
            async_it->second->set_level(level);
            return true;
        }
        return false;
    }

    auto flush_all() -> void {
        auto loggers = std::vector<Logger*>{};
        auto async_loggers = std::vector<AsyncLogger*>{};
        {
            std::lock_guard lock(mutex_);
            loggers.reserve(loggers_.size());
            for (auto& [name, logger] : loggers_) {
                loggers.push_back(logger.get());
            }
            async_loggers.reserve(async_loggers_.size());
            for (auto& [name, logger] : async_loggers_) {
                async_loggers.push_back(logger.get());
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
    Registry() = default;
    std::unordered_map<std::string, std::unique_ptr<Logger>> loggers_;
    std::unordered_map<std::string, std::unique_ptr<AsyncLogger>> async_loggers_;
    mutable std::mutex mutex_;
};

// 便捷自由函数
export auto create_logger(std::string name) -> Logger& {
    return Registry::instance().create(std::move(name));
}

export auto get_logger(std::string_view name) -> Logger* { return Registry::instance().get(name); }

}  // namespace ylog
