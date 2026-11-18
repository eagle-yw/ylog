cmake_minimum_required(VERSION 3.31 FATAL_ERROR)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
set(CMAKE_CXX_MODULE_STD ON)


project(Ylog VERSION 0.1.0 LANGUAGES CXX)

enable_testing()


set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# GCC16 静态反射 (P2996) 需要显式开启
add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,GNU>:-freflection>)

# For Clang, to ignore deprecated warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-deprecated-declarations)
endif()

add_subdirectory(src)
add_subdirectory(test)
add_subdirectory(example)