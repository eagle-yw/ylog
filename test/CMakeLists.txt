include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)


add_executable(unit_test unit_test.cpp)

target_link_libraries(unit_test 
    gtest 
    gtest_main
    ylog
)

add_test(NAME unit_test COMMAND unit_test)

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
