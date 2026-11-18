# Ylog

C++26 模块化日志库：`Logger` / `AsyncLogger` / `Registry`，pattern formatter，
stdout / 任意 ostream / 文件 / 大小轮转 / 按日 / 回调 / 环形缓冲 sink，以及丢弃全部输出的
`NullSink`；颜色按 sink 的 `ColorMode` 配置，只上在 pattern 的 `%^`/`%$` 区间上。
设计取舍见 `docs/adr/`，术语见 `GLOSSARY.md`。

## 在自己项目里使用

Ylog 以 C++26 模块发布：`cmake --install` 装的是**模块源码**，由消费方在本地编成 BMI。没有可跨机器分发的二进制包——BMI 与编译器、标准库、是否 `-fno-exceptions` 强绑定，换个机器就得重编。

```sh
cmake --preset gcc16
cmake --install out/build/gcc16 --prefix /your/prefix
```

依赖口径是硬约束。缺任何一条，`find_package` 会带着可直接照抄的修复片段失败，而不是让你去猜链接错误：

| 约束 | 要求 |
| --- | --- |
| CMake | **≥ 4.5**（`import std` 的实验 GUID 只在 4.5 的 toolchain 检测里被识别）|
| 编译器 | **GCC ≥ 16.2**；clang 未实现 P2996 静态反射，暂不支持（[#43](https://github.com/eagle-yw/ylog/issues/43)）|
| `import std` | 消费方顶层工程须自己开启，包配置无法代劳 |

最小工程：

```cmake
cmake_minimum_required(VERSION 4.5)

# import std 的实验 GUID 只能由顶层工程设置
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
set(CMAKE_CXX_MODULE_STD ON)

project(app LANGUAGES CXX)

find_package(Ylog 0.1 CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE Ylog::ylog)
```

```cpp
// main.cpp
import std;
import ylog;

int main() {
    auto logger = ylog::Logger("app");
    logger.info({}, "hello, {}", "ylog");

    // 自由函数作用于默认 logger：Registry 中名为 default 的常驻槽位（ADR-0010）
    ylog::info({}, "no logger object needed");
}
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/your/prefix
cmake --build build
```

`Ylog::ylog` 的接口属性里带着 `cxx_std_26`、`-freflection`，以及（若库以无异常构建）`YLOG_NO_EXCEPTIONS` 与 `-fno-exceptions`。**不要自行覆盖这些**：`exceptions_enabled` 在模块编译期定型，改了会与 BMI 口径不一致。

只在本仓库内开发时仍可直接 `add_subdirectory`；`Ylog::ylog` 与裸名 `ylog` 指向同一目标。各能力的可运行示例见 `example/`——每项能力一个编译单元，入口清单在 `demo.cppm`。

## 构建要求

工具链版本是硬约束，CI（`.github/workflows/ci.yml`）与本地必须一致：

| 组件 | 版本 | 原因 |
| --- | --- | --- |
| CMake | **4.5+** | `import std` 的实验 GUID 只在 4.5 的 toolchain 检测里被识别，4.4 及以下 configure 直接失败 |
| GCC | **16.2+** | `import std`、静态反射（`-freflection`）、`std::format`；16.0.1 读取聚合模块 BMI 会报 `Bad file data` |
| Ninja | 任意近期版本 | 生成器 |
| clang-format | **23.1.x** | 换行与对齐决策跨小版本会变，版本漂移会让 CI 与本地报不同结果 |

CI 的所有作业都跑在 `archlinux:latest` 容器里，工具链直接来自 Arch 仓库，
与本地 Arch 环境同源。Ubuntu 不作为基础镜像：24.04 的 toolchain PPA 只提供 GCC 16 的
`16-20260315` 快照（16.0.1），它在读取 `export module ylog;` 这类聚合模块的 BMI 时报
`failed to read compiled module: Bad file data`，所有 `import ylog` 的编译单元全部失败。

升级 CMake 时须同步替换 `CMakeLists.txt` 顶部的 `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` GUID。

## 构建与测试

```sh
cmake --preset gcc16
cmake --build --preset gcc16
ctest --test-dir out/build/gcc16 --output-on-failure
```

无异常变体（产物开关，会以 `-fno-exceptions` 编译 `ylog`）：

```sh
cmake --preset gcc16-noexc
cmake --build --preset gcc16-noexc
ctest --test-dir out/build/gcc16-noexc --output-on-failure
```

异常开启的构建默认还会编一份 `ylog_noexc` 并用同一个自检程序覆盖，
单次 `ctest` 就能同时验证两种模式（`YLOG_BUILD_NOEXC_TESTS=OFF` 可关闭）。

## 格式与静态检查

```sh
# Arch：clang-format 随 clang 包
sudo pacman -S clang
clang-format -i $(find src example test -type f \( -name '*.cppm' -o -name '*.cpp' \))
```

非 Arch 环境用 `python3 -m pip install clang-format==23.1.5`，须与 CI 的小版本一致。

`clang-tidy` 目前跑不起来，也不在 CI 里：clang 23 尚未实现 P2996 静态反射
（`^^T` / `std::meta::*`），`src/message.cppm` 无法解析，而其余编译单元都要拿它的
BMI。配置留在 `.clang-tidy`，等 clang 支持反射后恢复。

clang 侧的缺口（clang-tidy、Clang 构建矩阵）是**主动暂缓**，不是遗漏：
本仓库优先试最新语言特性，不为下游工具链退化实现。解除条件与解锁后的动作记在
[#43](https://github.com/eagle-yw/ylog/issues/43)，clang 落地 P2996 后按那张清单回来处理。
