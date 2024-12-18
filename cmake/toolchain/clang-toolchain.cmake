# 设置编译器路径
set(CMAKE_C_COMPILER "/usr/sbin/clang")
set(CMAKE_CXX_COMPILER "/usr/sbin/clang++")

# # 设置LLVM的bin、include和lib路径
# set(LLVM_DIR "/usr")
# set(CMAKE_PREFIX_PATH "${LLVM_DIR}")

# # 设置头文件路径
# include_directories(${LLVM_DIR}/include)

# # 设置库路径
# link_directories(${LLVM_DIR}/lib)

# 设置链接时优先使用LLVM的库而不是系统库
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
