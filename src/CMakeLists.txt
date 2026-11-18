# 模块源文件清单：正常构建与无异常变体共用，避免两处清单漂移。
set(YLOG_MODULE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ylog.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/level.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/message.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/formatter.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/logger.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/async.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/registry.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/detail/errors.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/detail/os.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_stdout.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_file.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_file_rotating.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/sinks/sink_file_daily.cppm
)

add_library(ylog)

target_sources(ylog
    PUBLIC FILE_SET CXX_MODULES FILES
    ${YLOG_MODULE_SOURCES}
)

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

# 测试变体：只在本仓库的异常开启构建里出现，供单次 ctest 双覆盖。
if(YLOG_BUILD_NOEXC_TESTS AND NOT YLOG_NO_EXCEPTIONS)
    add_library(ylog_noexc)

    target_sources(ylog_noexc
        PUBLIC FILE_SET CXX_MODULES FILES
        ${YLOG_MODULE_SOURCES}
    )

    ylog_enable_no_exceptions(ylog_noexc)
endif()
