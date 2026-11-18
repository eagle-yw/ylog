# 模块源文件清单：正常构建与无异常变体共用，避免两处清单漂移。
set(YLOG_MODULE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ylog.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/level.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/message.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/formatter.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/logger.cppm
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

# 无异常变体：exceptions_enabled 在模块编译期定型，因此必须整份源码重新编译，
# 只在消费端加 -fno-exceptions 不会生效。
if(YLOG_BUILD_NOEXC_TESTS)
    add_library(ylog_noexc)

    target_sources(ylog_noexc
        PUBLIC FILE_SET CXX_MODULES FILES
        ${YLOG_MODULE_SOURCES}
    )

    target_compile_definitions(ylog_noexc PUBLIC YLOG_NO_EXCEPTIONS)
    target_compile_options(ylog_noexc PUBLIC -fno-exceptions)
endif()
