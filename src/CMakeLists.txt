
add_library(ylog)

target_sources(ylog 
    PUBLIC FILE_SET CXX_MODULES FILES
    ylog.cppm
    level.cppm
    logger.cppm
    message.cppm
    sinks/sink.cppm
    sinks/sink_file.cppm
    sinks/sink_stdout.cppm
)

