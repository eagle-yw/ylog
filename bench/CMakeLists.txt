# 基准程序：由 YLOG_BUILD_BENCH 控制，默认不构建。
# 必须在 Release（或 RelWithDebInfo）下跑——gcc16 preset 是 Debug，数字没有意义；
# 见 CMakePresets.json 里的 gcc16-bench。

find_package(Threads REQUIRED)

# 优先用系统包；缺失时拉一份，且不安装、不编它自带的测试。
find_package(benchmark CONFIG QUIET)
if(NOT benchmark_FOUND)
    include(FetchContent)

    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.1
    )

    # 根目录把 -freflection 加成了目录级选项，而 GCC 只在 -std=c++26 下接受它；
    # Google Benchmark 按 C++17 编译，继承这个开关会直接报错。拉取期间摘掉目录
    # 选项，结束后恢复——本目录自己的目标在恢复之后才创建，不受影响。
    get_property(bench_parent_compile_options DIRECTORY PROPERTY COMPILE_OPTIONS)
    set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
    FetchContent_MakeAvailable(googlebenchmark)
    set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "${bench_parent_compile_options}")
endif()

# 四个程序共用的支撑模块：计时、结果输出、组件工厂、行数校验。
add_library(bench_support STATIC)

target_sources(bench_support
    PUBLIC FILE_SET CXX_MODULES FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/support.cppm
)

target_link_libraries(bench_support PUBLIC ylog)

add_executable(bench_throughput throughput.cpp)
target_link_libraries(bench_throughput PRIVATE bench_support)

add_executable(bench_async async_bench.cpp)
target_link_libraries(bench_async PRIVATE bench_support)

add_executable(bench_latency latency_bench.cpp)
target_link_libraries(bench_latency PRIVATE bench_support benchmark::benchmark)

add_executable(bench_formatter formatter_bench.cpp)
target_link_libraries(bench_formatter PRIVATE bench_support benchmark::benchmark)

add_executable(bench_io io_bench.cpp)
target_link_libraries(bench_io PRIVATE bench_support benchmark::benchmark)
