// 平台差异的唯一集中点：线程 id、进程 id、UTC 偏移。
// 标准库不提供可用的数值接口（std::thread::id 不可取数且哈希不保证不碰撞、
// 没有进程设施、std::tm 没有标准偏移字段），因此平台宏只出现在本模块。
module;

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <time.h>
#include <windows.h>
#else
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

export module ylog_detail_os;

import std;

namespace ylog_detail_os {

// 线程 id（%t）：与 spdlog 的 os::thread_id() 一致，Linux 上取内核 TID，
// 可与 ps -T/top -H 对照；异步 sink 因此报告生产者线程而不是工作线程。
export auto thread_id() -> std::size_t {
#if defined(_WIN32)
    return static_cast<std::size_t>(::GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<std::size_t>(::syscall(SYS_gettid));
#else
    // 兜底：仅保证同一线程稳定，可能与其它线程碰撞，也不对应内核 TID。
    return static_cast<std::size_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

// 进程 id（%P）：与 spdlog 一致，每次格式化时读取。
export auto process_id() -> std::uint32_t {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

// UTC 偏移（分钟），%z 使用。
// 不用 C++20 tzdb：MSVC 未实现，且依赖运行时 tzdata、取不到时会抛异常；
// 这里与 spdlog 同为 POSIX 的 tm_gmtoff（Windows 用 mktime/_mkgmtime 差值）。
export auto utc_offset_minutes(const std::tm& time) -> int {
#if defined(_WIN32)
    auto local_tm = time;
    auto local_seconds = std::mktime(&local_tm);
    auto utc_seconds = ::_mkgmtime(&local_tm);
    if (local_seconds == -1 || utc_seconds == -1) {
        return 0;
    }
    return static_cast<int>((utc_seconds - local_seconds) / 60);
#else
    return static_cast<int>(time.tm_gmtoff / 60);
#endif
}

// stdout / stderr 是否连着终端。颜色只在终端上有意义，所以 Automatic 在配置期用这里
// 定型一次，热路径不再调用 isatty；不区分终端是否支持颜色（不做 Windows 控制台 API）。
export auto stdout_is_tty() -> bool {
#if defined(_WIN32)
    return ::_isatty(::_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

export auto stderr_is_tty() -> bool {
#if defined(_WIN32)
    return ::_isatty(::_fileno(stderr)) != 0;
#else
    return ::isatty(STDERR_FILENO) != 0;
#endif
}

}  // namespace ylog_detail_os
