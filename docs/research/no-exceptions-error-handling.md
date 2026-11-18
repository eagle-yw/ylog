# 无异常构建下的 C++ 库错误报告：通用优秀实践调研

调研问题：C++ 库同时支持「启用异常」与「`-fno-exceptions` 构建」时，配置失败与运行期错误应如何报告？Ylog 该选集中宏、生成配置，还是 `std::expected` 工厂？

## 结论摘要

1. **分层错误模型是主流共识**：构造/配置失败是 fatal，可以抛异常；日志热路径失败不抛，交给回调或返回值。spdlog 官方明确写明「logging 时不抛、构造时可能抛」，并用 `err_handler` 把错误交给调用方，默认写 stderr 且限流。
2. **主流库用「集中式宏」隔离异常语法**：spdlog 的 `SPDLOG_*`、fmt 的 `FMT_TRY`/`FMT_CATCH`、nlohmann/json 的 `JSON_TRY`/`JSON_CATCH`/`JSON_THROW` 都把 `try`/`throw`/`catch` 收拢到一两个头文件里，业务代码只调用包装宏/函数。Ylog 当前的做法方向正确，缺点是宏散落在两个 sink 中。
3. **「自动探测编译器」比「要求用户定义宏」更友好**：fmt 和 nlohmann/json 会根据 `__cpp_exceptions` / `__EXCEPTIONS` / `_CPPUNWIND` 自动推断是否支持异常，同时保留用户强制覆盖的开关。
4. **禁用异常常常是项目级约束，而非库级需求**：Google 风格指南与 LLVM 编码标准都从整个代码库层面禁用异常，理由是二进制体积、控制流可读性、与既有代码集成。这不等于每个库都应默认禁用异常，但意味着**库不能假设调用方一定启用了异常**。
5. **`std::expected` 是方案 C 的标准化形态，但不是零成本**：它把「失败」变成返回类型的一部分，可作为异常世界与 nothrow 世界之间的桥；但它要求错误类型可无抛出移动，且 `value()` 在错误状态下仍会抛 `std::bad_expected_access`。

## 一手来源与做法

### 1. spdlog：分层 + `err_handler` + 集中宏

官方 Wiki「Error handling」原文：

> “Spdlog will not throw exceptions while logging (since version 39cdd08). It might throw during the construction of a logger or sink, because it is considered fatal.”
>
> “If an error happens during logging, the library will print an error message to stderr.”
>
> “To avoid flooding the screen with error messages, the rate is limited per logger to 1 message/minute.”

来源：<https://github.com/gabime/spdlog/wiki/Error-handling>

源码层面，`include/spdlog/logger-inl.h` 把「捕获异常 → 转交 err_handler」写成一个宏，热路径只写 `SPDLOG_TRY { sink->log(msg); } SPDLOG_LOGGER_CATCH(msg.source)`：

```cpp
#ifndef SPDLOG_NO_EXCEPTIONS
#    define SPDLOG_TRY try
#    define SPDLOG_THROW(exception) throw exception
#    define SPDLOG_LOGGER_CATCH(location)                              \
        catch (const std::exception &ex) { ... err_handler_(...); }    \
        catch (...) { ... err_handler_(...); }
#else
#    define SPDLOG_TRY
#    define SPDLOG_THROW(exception) ::std::abort()
#    define SPDLOG_CATCH_STD
#    define SPDLOG_LOGGER_CATCH(location)
#endif
```

来源：<https://raw.githubusercontent.com/gabime/spdlog/v1.x/include/spdlog/logger-inl.h>

`include/spdlog/common.h` 中还有同族的 `SPDLOG_TRY` / `SPDLOG_THROW` / `SPDLOG_CATCH_STD`；不同版本的 `SPDLOG_THROW` 在 `abort()` 前是否打印 `spdlog fatal error:` 略有差异，但共同点是**不调用 `throw`**。来源：<https://raw.githubusercontent.com/gabime/spdlog/v1.x/include/spdlog/common.h>

要点：spdlog 与 Ylog ADR 0005 的分层决策几乎一致；差异在于 spdlog 把异常语法全部收进宏，且默认限流是「每 logger 每分钟一条」，Ylog 是「每 sink 每秒一条」。

### 2. fmt：宏包装 + 编译器自动探测

`include/fmt/core.h`（其中标识 `FMT_VERSION 120202`）原文：

```cpp
#ifdef FMT_USE_EXCEPTIONS
  // Use the provided definition.
#elif defined(__GNUC__) && !defined(__EXCEPTIONS)
# define FMT_USE_EXCEPTIONS 0
#elif defined(__clang__) && !defined(__cpp_exceptions)
# define FMT_USE_EXCEPTIONS 0
#elif FMT_MSC_VERSION && !_HAS_EXCEPTIONS
# define FMT_USE_EXCEPTIONS 0
#else
# define FMT_USE_EXCEPTIONS 1
#endif

#if FMT_USE_EXCEPTIONS
# define FMT_TRY try
# define FMT_CATCH(x) catch (x)
#else
# define FMT_TRY if (true)
# define FMT_CATCH(x) if (false)
#endif
```

并在析构函数里这样使用：

```cpp
~iterator_buffer() {
  // Don't crash if flush fails during unwinding.
  FMT_TRY { flush(); }
  FMT_CATCH(...) {}
}
```

来源：<https://raw.githubusercontent.com/fmtlib/fmt/master/include/fmt/core.h>

要点：`FMT_TRY`/`FMT_CATCH` 展开为 `if (true)`/`if (false)`，从根源上消除 `try`/`catch` 关键字；用户可先定义 `FMT_USE_EXCEPTIONS` 覆盖自动探测。错误上报走 `FMT_NORETURN void report_error(const char* message)`，把「如何失败」与「用什么语法失败」解耦。

### 3. nlohmann/json：集中宏 + 用户钩子 + 用后清理

`include/nlohmann/detail/macro_scope.hpp` 原文：

```cpp
// allow disabling exceptions
#if (defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)) && !defined(JSON_NOEXCEPTION)
#define JSON_THROW(exception) throw exception
#define JSON_TRY try
#define JSON_CATCH(exception) catch(exception)
#define JSON_INTERNAL_CATCH(exception) catch(exception)
#else
#include <cstdlib>
#define JSON_THROW(exception) std::abort()
#define JSON_TRY if(true)
#define JSON_CATCH(exception) if(false)
#define JSON_INTERNAL_CATCH(exception) if(false)
#endif
```

并允许 `JSON_THROW_USER` / `JSON_TRY_USER` / `JSON_CATCH_USER` / `JSON_INTERNAL_CATCH_USER` 覆盖默认行为；文件注释要求在 `json.hpp` 末尾包含 `macro_unscope.hpp` 撤销这些宏，避免污染用户宏空间。

来源：<https://raw.githubusercontent.com/nlohmann/json/develop/include/nlohmann/detail/macro_scope.hpp>

要点：这是「集中式宏」的教科书范例：一处定义、自动探测、用户可注入、用后 undef。Ylog 若采用集中宏方案，可借鉴「自动探测 + 用户覆盖 + 作用域清理」三点。

### 4. Google C++ Style Guide：项目级禁用异常，并承认工厂函数替代方案

官方原文：

> “We do not use C++ exceptions.”
>
> “Exceptions are the only way for a constructor to fail. We can simulate this with a factory function or an Init() method, but these require heap allocation or a new ‘invalid’ state, respectively.”
>
> “Our advice against using exceptions is not predicated on philosophical or moral grounds, but practical ones.”

来源：<https://google.github.io/styleguide/cppguide.html#Exceptions>

要点：这段直接对应方案 C——**工厂函数是业界公认的「构造函数失败」替代方案**，但代价是需要堆分配或引入「无效对象」状态。同时 Google 的立场是「由项目整体决定不用异常」，而不是让每个库都提供双模式。

### 5. LLVM Coding Standards：禁用异常的官方理由

官方原文：

> “In an effort to reduce code and executable size, LLVM does not use exceptions or RTTI (runtime type information, for example, dynamic_cast<>).”

来源：<https://releases.llvm.org/15.0.0/docs/CodingStandards.html#do-not-use-rtti-or-exceptions>

要点：LLVM 的动机是代码与可执行文件体积。它说明了「为什么世界上有 `-fno-exceptions` 用户」，但不构成「日志库必须默认无异常」的论据。

### 6. C++ Core Guidelines：错误策略要早定，失败用异常上报

官方页面可确认的规则：

> “I.10: USE EXCEPTIONS TO SIGNAL A FAILURE TO PERFORM A REQUIRED TASK”
>
> “Many traditional interface functions (e.g., UNIX signal handlers) use error codes (e.g., errno) to report what are really status codes, rather than errors. You don't have a good alternative to using such, so calling these does not violate the rule.”

来源：<https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#i10-use-exceptions-to-signal-a-failure-to-perform-a-required-task>

E 章节索引入口：<https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#S-error>

说明：官方单页过长，本次抓取在 F.6 处被截断，未能取得 E.1/E.12 的逐字正文；规则标题「E.12: Use noexcept when exiting a function because of a throw is impossible or unacceptable」可在多处引用中确认，逐字引用请以官方锚点 <https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#e12-use-noexcept-when-exiting-a-function-because-of-a-throw-is-impossible-or-unacceptable> 为准。此处不作为强证据使用。

### 7. `std::expected`：把失败变成返回值

cppreference 原文：

> “The class template std::expected provides a way to represent either of two values: an expected value of type T, or an unexpected value of type E. expected is never valueless.”

来源：<https://en.cppreference.com/w/cpp/utility/expected>

WG21 P0323R10 的设计动机原文：

> “C++'s two main error mechanisms are exceptions and return codes.”
>
> “Error visibility: Failure cases should appear throughout the code review.”
>
> “Clean code: Treatment of errors should be in a separate layer of code and as invisible as possible.”
>
> “Non-Intrusive error: Errors should not monopolize the communication channel dedicated to normal code flow.”
>
> “expected<T, E> can also act as a bridge between an exception-oriented code and a nothrow world.”
>
> “In order to ensure the never empty guaranties, expected<T, E> requires E to be no throw move constructible. This is normal as the E stands for an error, and throwing while reporting an error is a very bad thing.”
>
> “The value() function has a wide contract. If the expected object does not contain a value, an exception is thrown.”

来源：<https://open-std.org/JTC1/SC22/WG21/docs/papers/2021/p0323r10.html>

要点：`std::expected` 正好命中 Ylog 的两难——它既能让配置失败显式出现在签名里，又能在 no-throw 子系统中传递；但「错误类型必须不抛移动」和「`value()` 仍会抛」是两个硬约束，采用前需核实目标平台对 C++23 `<expected>` 的支持。

## 对 Ylog 三种方案的适配判断

| 方案 | 与现有规范的关系 | 与主流实践的关系 | 改动面 |
|------|----------------|----------------|--------|
| A 集中 partition + 函数模板 `if constexpr` | 完全兼容 `CODING_STANDARDS.md` 与 ADR 0005 | 对应 spdlog/fmt/nlohmann 的集中化思路，但用模板代替宏包装语法 | 新增 1 个 partition，改 2 个 sink，CMake 加 1 行 |
| B CMake `configure_file` 生成配置常量 | 同上 | 对应 fmt 的「可配置开关」，源码零 `#ifdef` | 新增 CMake 模板与生成步骤 |
| C `std::expected` 工厂 | **冲突**：`CODING_STANDARDS.md:14-16` 要求配置期抛异常，ADR 0005 第 5 行同义 | 对应 Google 承认的工厂替代方案与 P0323 的设计目标 | 改公开 API、改 ADR/规范、重写 `test/unit_test.cpp` 中多处 `EXPECT_THROW`（如 173、420、528、538、630、897、900 行） |

本地已验证：GCC 16.2.1 与 Clang 23.1.5 在 `-fno-exceptions` 下，普通函数里 `if constexpr(false)` 分支中的 `throw`/`try` 仍报编译错误；但**由模板参数控制**的丢弃分支可以通过。因此方案 A 的模板化收口在这两个编译器上可行，且比宏方案更易读、无宏污染。

推荐顺序：**A 优先**（改动最小、与现有 ADR 一致），若明确要求源码零 `#ifdef` 再叠加 B；C 只有在决定把「配置期抛异常」这条规范整体反转时才做，不应为了消除宏而顺带做 C。

## 参考链接清单

- spdlog 错误处理 Wiki：<https://github.com/gabime/spdlog/wiki/Error-handling>
- spdlog `logger-inl.h`：<https://raw.githubusercontent.com/gabime/spdlog/v1.x/include/spdlog/logger-inl.h>
- spdlog `common.h`：<https://raw.githubusercontent.com/gabime/spdlog/v1.x/include/spdlog/common.h>
- fmt `core.h`：<https://raw.githubusercontent.com/fmtlib/fmt/master/include/fmt/core.h>
- nlohmann/json `macro_scope.hpp`：<https://raw.githubusercontent.com/nlohmann/json/develop/include/nlohmann/detail/macro_scope.hpp>
- Google C++ Style Guide（Exceptions）：<https://google.github.io/styleguide/cppguide.html#Exceptions>
- LLVM Coding Standards：<https://releases.llvm.org/15.0.0/docs/CodingStandards.html#do-not-use-rtti-or-exceptions>
- C++ Core Guidelines（E 章节）：<https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#S-error>
- C++ Core Guidelines I.10：<https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#i10-use-exceptions-to-signal-a-failure-to-perform-a-required-task>
- cppreference `std::expected`：<https://en.cppreference.com/w/cpp/utility/expected>
- WG21 P0323R10 `std::expected`：<https://open-std.org/JTC1/SC22/WG21/docs/papers/2021/p0323r10.html>
