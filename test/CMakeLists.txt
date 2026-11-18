# 无异常构建只跑自检程序：gtest 的 GTEST_HAS_EXCEPTIONS 与本工程的 -fno-exceptions
# 口径不一致，且 unit_test 里有多处 EXPECT_THROW 断言配置期抛异常。
if(YLOG_NO_EXCEPTIONS)
    add_executable(noexc_test noexc_test.cpp)

    target_link_libraries(noexc_test PRIVATE ylog)

    add_test(NAME noexc COMMAND noexc_test)

    # 并发冒烟在 Block 策略下若唤醒丢失会挂住而不是失败，用超时兜底。
    set_tests_properties(noexc PROPERTIES TIMEOUT 60)
    return()
endif()

include(FetchContent)

# 测试依赖只服务本仓库：关掉它们自带的安装规则，否则 cmake --install 会把
# gtest/gmock 一并塞进 ylog 的安装前缀。
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)


# 按测试套件拆分：每类能力一个 TU，共享夹具在 test_support 模块里。
add_executable(unit_test
    logger_test.cpp
    formatter_test.cpp
    async_logger_test.cpp
    registry_test.cpp
    sink_test.cpp
    sink_ostream_test.cpp
    sink_color_test.cpp
    sink_file_test.cpp
    sink_rotating_test.cpp
    sink_daily_test.cpp
    sink_callback_test.cpp
    sink_ringbuffer_test.cpp
)

target_sources(unit_test
    PRIVATE FILE_SET CXX_MODULES FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_support.cppm
)

target_link_libraries(unit_test
    gtest
    gtest_main
    ylog
)

add_test(NAME unit_test COMMAND unit_test)

# default 保留名的 async-first 时序需要一个未被污染的 Registry：unit_test 是共享进程，
# 其中的 default 槽位多半已经创建。这个可执行文件只跑这一组用例。
# 本用例依赖异常，noexc 分支在上方已 return()，无需额外处理。
add_executable(registry_fresh_test registry_fresh_test.cpp)

target_link_libraries(registry_fresh_test
    gtest
    gtest_main
    ylog
)

add_test(NAME registry_fresh_test COMMAND registry_fresh_test)

# pattern flag oracle 测试：以 spdlog 1.17.0 的实际输出为准绳（research/pattern-flags.md）。
# 系统 spdlog 不足 1.17 时从源码拉取；离线环境可用
# -DFETCHCONTENT_SOURCE_DIR_SPDLOG=/path/to/spdlog-1.17.0 指向本地副本。
find_package(spdlog 1.17 CONFIG QUIET)
if(NOT spdlog_FOUND)
    FetchContent_Declare(
      spdlog
      GIT_REPOSITORY https://github.com/gabime/spdlog.git
      GIT_TAG        v1.17.0
    )
    FetchContent_MakeAvailable(spdlog)
endif()

add_executable(pattern_oracle_test pattern_oracle_test.cpp)

target_link_libraries(pattern_oracle_test PRIVATE
    gtest
    gtest_main
    ylog
    spdlog::spdlog
)

# spdlog 头文件按系统头处理，避免第三方告警影响本工程的 -Werror 口径。
target_include_directories(pattern_oracle_test SYSTEM PRIVATE
    $<TARGET_PROPERTY:spdlog::spdlog,INTERFACE_INCLUDE_DIRECTORIES>
)

add_test(NAME pattern_oracle_test COMMAND pattern_oracle_test)

# 测试变体：主库开启异常时，用同一个自检程序覆盖无异常库。
# 不用 gtest：GTEST_HAS_EXCEPTIONS 的口径须与整库一致，混用会引入 ODR 风险。
if(TARGET ylog_noexc)
    add_executable(noexc_test noexc_test.cpp)

    target_link_libraries(noexc_test PRIVATE ylog_noexc)

    add_test(NAME noexc COMMAND noexc_test)

    # 并发冒烟在 Block 策略下若唤醒丢失会挂住而不是失败，用超时兜底。
    set_tests_properties(noexc PROPERTIES TIMEOUT 60)
endif()
