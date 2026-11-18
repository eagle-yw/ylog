export module ylog:registry;

import std;
import :level;
import :formatter;
import :sink;
import :sink.stdout;
import :logger;
import :async;
import ylog_detail_errors;

namespace ylog {

// 全局 logger 注册表。同步 Logger 与异步 AsyncLogger 共用一个命名空间。
//
// 命名规则（设计记录见 docs/adr/0007-registry-namespace-and-ownership.md）：
// - 一个名字只能属于一个组件；
// - create() 对同名同步 logger 幂等（返回既有实例，忽略新给的配置），撞上异步组件抛；
// - create_async() 撞上任何已用名字都抛；
// - get()/get_async() 按类型查找，名字属于另一类时返回 nullptr；
// - default 是保留名：从进程开始就属于同步 Logger，与槽位是否已经懒创建无关，
//   create_async("default", …) 无条件抛（见 private 的 DefaultSlot）。
//
// sink 集合在创建期定型（ADR-0011）：create 收完整的 LoggerConfig，之后只可逐 sink
// 改 pattern。集合为空合法，语义是丢弃。
//
// 所有权：Registry 是唯一持有者。drop()/drop_all() 先摘除、再在锁外析构，所以异步
// 组件的排空发生在 drop() 返回之前——不存在「最后一个外部持有者析构才停机」的模型。
//
// 并发契约：创建/摘除/配置属配置期操作；flush_all() 先取快照再逐个 flush，避免持锁
// 等待异步组件的屏障。
//
// 默认 logger 的保留名：保留在创建之前就生效，槽位本身懒创建——首次 default_logger() /
// create("default") 才构造，初始 Sink Set 恰好一个 StdoutSink（ADR-0011）；槽位常驻，不可
// 摘除（ADR-0010）。这三件事由 private 的 DefaultSlot 独家持有，create / create_async /
// drop / drop_all 都经它判定，不各自写一份字符串比较。
export inline constexpr std::string_view default_logger_name = "default";

export class Registry {
public:
    static auto instance() -> Registry& {
        static Registry registry;
        return registry;
    }

    // 创建并注册同步 logger。同名同步 logger 已存在则返回既有实例（幂等）。
    auto create(LoggerConfig config) -> Logger& {
        if (DefaultSlot::is_reserved(config.name)) {
            // 保留名只接受空配置：初始 sink 集合与级别都由 default_logger() 定型，外部指定
            // 即违约。级别改走 set_level(default_logger_name, …)，不在这里静默丢弃。
            if (!config.sinks.empty() || config.level != Level::Info) {
                ylog_detail_errors::fail_config<std::invalid_argument>(
                    "default 的配置由 default_logger() 定型（一个 StdoutSink、Info 级），"
                    "不能经 create 指定；改级别请用 set_level");
            }
            return default_logger();
        }
        std::lock_guard lock(mutex_);
        auto it = entries_.find(config.name);
        if (it == entries_.end()) {
            it = entries_.try_emplace(config.name).first;
        } else if (it->second.asynchronous) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                std::format("logger 名称已被异步组件占用: {}", config.name));
        }
        auto& entry = it->second;
        if (!entry.synchronous) {
            entry.synchronous = std::make_unique<Logger>(std::move(config));
        }
        return *entry.synchronous;
    }

    // 便利面：零 sink 的同步 logger（语义是丢弃）。要 sink 就显式给 LoggerConfig。
    auto create(std::string name) -> Logger& {
        return create(LoggerConfig{.name = std::move(name)});
    }

    // 默认 logger：保留名的唯一创建路径，初始 sink 集合就是一个 StdoutSink。
    // 懒创建与常驻都由 DefaultSlot 独家定义，因此 default_logger() 与 create("default")
    // 得到同一个实例，且不论谁先调用。
    auto default_logger() -> Logger& {
        std::lock_guard lock(mutex_);
        return DefaultSlot::obtain(entries_);
    }

    // 创建异步组件。保留名 default 一律拒绝；其余名字已被任何组件占用时同样抛。
    auto create_async(std::string name, AsyncLoggerConfig config) -> AsyncLogger& {
        // 保留名必须在查表之前拒绝：槽位尚未懒创建时 entries_ 里没有 default，只查
        // 「名字是否已占用」会放行；此后 drop("default") 恒 false、drop_all() 跳过它，
        // 这个异步组件就再也摘不掉、排不空。
        if (DefaultSlot::is_reserved(name)) {
            ylog_detail_errors::fail_config<std::invalid_argument>(
                "default 是保留名，只能作为同步 logger 经 default_logger() 获得");
        }
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
    // default 是常驻槽位——default_logger() 缓存了它的引用，摘除会让引用悬垂。
    auto drop(std::string_view name) -> bool {
        if (DefaultSlot::is_reserved(name)) {
            return false;
        }
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

    // 摘除其余全部条目，default 除外（它同样不可摘除，理由见 drop）。
    auto drop_all() -> void {
        auto removed = Table{};
        {
            std::lock_guard lock(mutex_);
            auto it = entries_.begin();
            while (it != entries_.end()) {
                if (DefaultSlot::is_reserved(it->first)) {
                    ++it;
                    continue;
                }
                removed.insert(entries_.extract(it++));
            }
        }
        // 局部表在此析构：异步组件逐个排空，期间不持注册表锁。
    }

    // 作用于已存在的条目，不触发 default 槽位的懒创建：没人用过默认 logger 时，这里
    // 不会替它建出来，创建时的级别仍由构造配置决定。
    auto set_level(Level level) -> void {
        std::lock_guard lock(mutex_);
        for (auto& [name, entry] : entries_) {
            entry.apply_level(level);
        }
    }

    // 按名称设置级别，作用于同步或异步组件；名称不存在返回 false。与上一个重载一样
    // 不触发懒创建——保留名已属于同步 Logger，但条目要等 default_logger() 才存在。
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

    // default 槽位的唯一策略：保留名 + 懒创建 + 常驻，三件事同源。
    // 删掉本类型会让不变量重新散落回创建（create / create_async）与摘除
    // （drop / drop_all）两条路径——这正是它存在的理由。
    struct DefaultSlot {
        // 保留名判定：名字从进程开始就属于同步 Logger，与槽位是否已经构造无关。
        static auto is_reserved(std::string_view name) -> bool {
            return name == default_logger_name;
        }

        // 懒创建：调用方持锁。已存在则返回既有实例；不存在则按 ADR-0011 定型 sink 集合
        // 后插入，不预占空条目。
        static auto obtain(Table& entries) -> Logger& {
            auto it = entries.find(std::string(default_logger_name));
            if (it != entries.end()) {
                // 正常路径不可达：create_async 用 is_reserved() 无条件拒绝保留名，槽位只能
                // 由本函数以同步 Logger 填充。保留这条分支，让绕过策略的改动立刻响亮失败，
                // 而不是静默返回空指针。
                if (!it->second.synchronous) {
                    ylog_detail_errors::fail_config<std::invalid_argument>(
                        "default 槽位被非同步组件占用：保留名只能经 Registry::default_logger() "
                        "创建");
                }
                return *it->second.synchronous;
            }
            auto created = std::make_unique<Logger>(
                LoggerConfig{.name = std::string(default_logger_name),
                             .sinks = make_sinks(AnySink{StdoutSink{}, default_pattern})});
            auto& reference = *created;
            entries.try_emplace(std::string(default_logger_name)).first->second.synchronous =
                std::move(created);
            return reference;
        }
    };

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
