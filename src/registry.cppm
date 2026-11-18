export module ylog:registry;

import std;
import :logger;
import :sink;
import :sink.stdout;

namespace ylog {

// 全局 logger 注册表。提供按名称创建、查找、管理 logger。
export class Registry {
public:
    static auto instance() -> Registry& {
        static Registry registry;
        return registry;
    }

    // 创建新 logger 并注册。同名已存在则返回既有实例。
    auto create(std::string name) -> Logger& {
        std::lock_guard lock(mutex_);
        auto [it, inserted] = loggers_.try_emplace(name, nullptr);
        if (inserted) {
            it->second = std::make_unique<Logger>(name);
        }
        return *it->second;
    }

    // 按名称查找，不存在返回 nullptr。
    auto get(std::string_view name) -> Logger* {
        std::lock_guard lock(mutex_);
        auto it = loggers_.find(std::string(name));
        return it != loggers_.end() ? it->second.get() : nullptr;
    }

    auto contains(std::string_view name) -> bool {
        return get(name) != nullptr;
    }

    auto drop(std::string_view name) -> bool {
        std::lock_guard lock(mutex_);
        return loggers_.erase(std::string(name)) > 0;
    }

    auto drop_all() -> void {
        std::lock_guard lock(mutex_);
        loggers_.clear();
    }

    auto set_level(Level level) -> void {
        std::lock_guard lock(mutex_);
        for (auto& [name, logger] : loggers_) {
            logger->set_level(level);
        }
    }

    auto flush_all() -> void {
        std::lock_guard lock(mutex_);
        for (auto& [name, logger] : loggers_) {
            logger->flush();
        }
    }

    Registry(const Registry&) = delete;
    auto operator=(const Registry&) -> Registry& = delete;

private:
    Registry() = default;
    std::unordered_map<std::string, std::unique_ptr<Logger>> loggers_;
    mutable std::mutex mutex_;
};

// 便捷自由函数
export auto create_logger(std::string name) -> Logger& {
    return Registry::instance().create(std::move(name));
}

export auto get_logger(std::string_view name) -> Logger* {
    return Registry::instance().get(name);
}

} // namespace ylog
