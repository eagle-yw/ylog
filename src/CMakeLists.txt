# 模块源文件清单：正常构建与无异常变体共用，避免两处清单漂移。
set(YLOG_MODULE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ylog.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/level.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/message.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/formatter.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/logger_base.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/logger.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/async.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/async_queue.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/registry.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/logging.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/detail/errors.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/detail/os.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_stdout.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_ostream.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_file.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_file_lifecycle.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_file_rotating.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_file_daily.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_callback.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_ringbuffer.cppm
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(ylog)
add_library(Ylog::ylog ALIAS ylog)

target_sources(ylog
    PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${YLOG_MODULE_SOURCES}
)

# 接口上必须带语言标准与反射开关：模块源码随包安装、由消费方本地编成 BMI，
# 缺其中任何一项，消费方编译 ylog 的接口单元就会失败。
target_compile_features(ylog PUBLIC cxx_std_26)
target_compile_options(ylog PUBLIC $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-freflection>)

# exceptions_enabled 在模块编译期定型，因此开关必须作用在库本身，而不是消费端。
# 宏随接口传播，保证消费端看到的口径与 BMI 一致。
function(ylog_enable_no_exceptions target)
    target_compile_definitions(${target} PUBLIC YLOG_NO_EXCEPTIONS)
    target_compile_options(${target} PUBLIC
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions>
        $<$<CXX_COMPILER_ID:MSVC>:/EHs-c->
    )
endfunction()

if(YLOG_NO_EXCEPTIONS)
    ylog_enable_no_exceptions(ylog)
endif()

# 测试变体：只在本仓库的异常开启构建里出现，供单次 ctest 双覆盖；不安装。
if(YLOG_BUILD_NOEXC_TESTS AND NOT YLOG_NO_EXCEPTIONS)
    add_library(ylog_noexc)

    target_sources(ylog_noexc
        PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES ${YLOG_MODULE_SOURCES}
    )

    target_compile_features(ylog_noexc PUBLIC cxx_std_26)
    target_compile_options(ylog_noexc PUBLIC $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-freflection>)
    ylog_enable_no_exceptions(ylog_noexc)
endif()

# 安装面：模块源码 + 静态库 + 包配置。不支持二进制分发——BMI 与编译器、标准库、
# YLOG_NO_EXCEPTIONS 口径强绑定，跨机器不可移植。
install(TARGETS ylog
    EXPORT YlogTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ylog
)

install(EXPORT YlogTargets
    NAMESPACE Ylog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Ylog
)

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/YlogConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/YlogConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Ylog
)

# 0.x 没有 API 稳定承诺，SameMajorVersion 在 0.x 上等于放行 0.7 → 0.1。
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/YlogConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/YlogConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/YlogConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Ylog
)
