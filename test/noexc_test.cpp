// 无异常构建（-fno-exceptions + YLOG_NO_EXCEPTIONS）的独立 ctest target。
// 只覆盖不抛异常的路径：正常配置、级别过滤，以及热路径失败交给 err_handler。
// 不使用 gtest：gtest 的 GTEST_HAS_EXCEPTIONS 口径须与整份库一致，混用会引入 ODR 风险。

import std;
import ylog;
import ylog_detail_errors;

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#error "noexc_test 必须以 -fno-exceptions 编译"
#endif

// 打开异常后配置失败走 throw，因此这里只能是编译期确认：开关确实生效。
static_assert(!ylog_detail_errors::exceptions_enabled);

namespace {

auto failures = int{0};

auto check(bool condition, std::string_view what) -> void {
    if (!condition) {
        std::println(std::cerr, "FAILED: {}", what);
        ++failures;
    }
}

struct Capture {
    std::string* out;
    auto write(std::string_view line) -> void { *out += line; }
};

auto temp_directory() -> std::filesystem::path {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() / std::format("ylog-noexc-{}", stamp);
    std::filesystem::create_directories(path);
    return path;
}

auto read_all(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// 热路径失败必须走 err_handler，而不是 throw / terminate。
auto reports_hot_path_failure() -> void {
    if (!std::filesystem::exists("/dev/full")) {
        std::println("skip: /dev/full unavailable");
        return;
    }

    auto reported = std::string{};
    auto config = ylog::RotatingFileConfig{};
    config.filename = "/dev/full";
    config.err_handler = [&reported](std::string_view message) { reported = std::string(message); };

    auto sink = ylog::RotatingFileSink(config);
    sink.write("hello\n");
    sink.flush();
    check(!reported.empty(), "hot path failure reaches err_handler");
}

}  // namespace

auto main() -> int {
    auto logger = ylog::Logger("noexc");
    logger.set_level(ylog::Level::Info);

    auto out = std::string{};
    logger.append_sink(Capture{&out}, "%v");
    logger.info({}, "hello {}", 42);
    check(out == "hello 42\n", "info message reaches sink");

    out.clear();
    logger.debug({}, "filtered");
    check(out.empty(), "below-level message is filtered");

    // 配置成功路径：无异常构建下正常构造并写出。
    auto directory = temp_directory();
    auto path = directory / "app.log";
    {
        auto sink = ylog::FileSink(path.string());
        sink.write("file\n");
        sink.flush();
    }
    check(read_all(path) == "file\n", "file sink writes");

    auto rotating_config = ylog::RotatingFileConfig{};
    rotating_config.filename = (directory / "rotating.log").string();
    rotating_config.max_size = 16;
    {
        auto sink = ylog::RotatingFileSink(rotating_config);
        sink.write("rotating\n");
        sink.flush();
    }
    check(read_all(directory / "rotating.log") == "rotating\n", "rotating sink writes");
    std::filesystem::remove_all(directory);

    auto& registry = ylog::Registry::instance();
    auto& named = registry.create("noexc-app");
    named.set_level(ylog::Level::Debug);
    check(registry.get("noexc-app") == &named, "registry stores logger");
    check(registry.drop("noexc-app"), "registry drops logger");

    reports_hot_path_failure();

    if (failures != 0) {
        std::println(std::cerr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("noexc checks passed");
    return 0;
}
