add_library(ylog)

target_sources(ylog
    PUBLIC FILE_SET CXX_MODULES FILES
    ylog.cppm
    level.cppm
    message.cppm
    formatter.cppm
    logger.cppm
    registry.cppm
    detail/errors.cppm
    sinks/sink.cppm
    sinks/sink_stdout.cppm
    sinks/sink_file.cppm
    sinks/sink_file_rotating.cppm
    sinks/sink_file_daily.cppm
)
