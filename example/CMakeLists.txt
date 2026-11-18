# 每个能力一个实现单元（`module demo;`），入口清单在 demo.cppm 里导出。
add_executable(demo
    demo_main.cpp
    demo_levels.cpp
    demo_reflect.cpp
    demo_pattern.cpp
    demo_sinks.cpp
    demo_color.cpp
    demo_runtime.cpp
)

target_sources(demo
    PRIVATE FILE_SET CXX_MODULES FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/demo.cppm
)

target_link_libraries(demo
    PRIVATE ylog
)
