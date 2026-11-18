# 最低版本由 `import std` 的实验 GUID 决定：GUID 只在 CMake 4.5 的 toolchain
# 检测里被识别，4.4 及以下会直接报 "CXX_MODULE_STD requires toolchain support"。
# CI 与本地都固定 4.5.0（README「构建要求」），升级 CMake 时须同步替换 GUID。
cmake_minimum_required(VERSION 4.5 FATAL_ERROR)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
set(CMAKE_CXX_MODULE_STD ON)


project(Ylog VERSION 0.1.0 LANGUAGES CXX)

enable_testing()


set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# GCC16 静态反射 (P2996) 需要显式开启；clang 尚未实现，故 Clang 构建矩阵暂缓（issue #43）
add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,GNU>:-freflection>)

# For Clang, to ignore deprecated warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-deprecated-declarations)
endif()

# 两个开关分工，不要混用：
# - YLOG_NO_EXCEPTIONS：产物开关。与源码宏同名，开启后 ylog 以 -fno-exceptions 编译
#   （配置失败改为 assert + 终止，热路径失败仍走 err_handler，
#   见 docs/research/no-exceptions-error-handling.md）。
# - YLOG_BUILD_NOEXC_TESTS：本仓库的测试开关。在异常开启的构建里额外编一份无异常库，
#   让单次 ctest 同时覆盖两种模式；对下游无意义，主库为无异常时自动跳过。
option(YLOG_NO_EXCEPTIONS "以 -fno-exceptions 构建 ylog" OFF)
option(YLOG_BUILD_NOEXC_TESTS "额外构建无异常变体与自检（YLOG_NO_EXCEPTIONS=OFF 时生效）" ON)

add_subdirectory(src)
add_subdirectory(test)
add_subdirectory(example)
