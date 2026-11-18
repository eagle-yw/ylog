# Ylog

C++26 模块化日志库：`Logger` / `AsyncLogger` / `Registry`，pattern formatter，
stdout / 任意 ostream / 文件 / 大小轮转 / 按日 sink，以及丢弃全部输出的 `NullSink`；
颜色按 sink 的 `ColorMode` 配置，只上在 pattern 的 `%^`/`%$` 区间上。
设计取舍见 `docs/adr/`，术语见 `GLOSSARY.md`。

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
