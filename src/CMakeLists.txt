
add_library(ylog)

target_sources(ylog 
    PUBLIC FILE_SET CXX_MODULES FILES
    ylog.cppm
    level.cppm
    logger.cppm
    message.cppm
    # sinks/sinks_file.cppm
)