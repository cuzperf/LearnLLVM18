cmake_minimum_required(VERSION 3.20)

project(LearnLLVM LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# LLVM 配置
# ============================================================================
if(NOT DEFINED LLVM_DIR)
    set(LLVM_DIR /usr/lib/llvm-18/lib/cmake/llvm)
endif()

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# 头文件路径与宏定义
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# 设置为 C++17（LLVM 18 要求的最低标准）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# 构建目标
# ============================================================================
add_executable(jit_demo main.cpp)

# 动态链接：直接链接 LLVM 导出目标（即 libLLVM-18.so）
target_link_libraries(jit_demo PRIVATE LLVM)

# 导出所有符号到动态符号表（JIT 调试必需）
set_target_properties(jit_demo PROPERTIES
    ENABLE_EXPORTS ON
)

# 宏定义：项目源码根目录（用于 JIT 调试信息中定位源文件）
target_compile_definitions(jit_demo PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# ============================================================================
# 静态库 demo：JIT 代码调用 .a 归档中实现的函数
# ============================================================================

# 把 jit_demo.cpp 编成静态库 libjitgcd.a，作为"外部函数实现"的载体
# POSITION_INDEPENDENT_CODE = -fPIC：JIT 分配的可执行内存位置不固定，
# PIC 可避免小代码模型重定位溢出
add_library(jitgcd STATIC jit_demo.cpp)
set_target_properties(jitgcd PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(jit_staticlibrary_demo jit_staticlibrary_demo.cpp)

# 只链接 LLVM，故意【不】链接 jitgcd：进程的符号表里没有 gcd，
# 这样 demo 能跑通就证明符号确实是从 .a 归档中解析出来的
target_link_libraries(jit_staticlibrary_demo PRIVATE LLVM)

# 宏定义：归档文件的完整路径（$<TARGET_FILE:jitgcd> 是 CMake 生成表达式，
# 展开为 libjitgcd.a 的实际输出路径）
target_compile_definitions(jit_staticlibrary_demo PRIVATE
    JITGCD_ARCHIVE_PATH="$<TARGET_FILE:jitgcd>"
)

# 保证先产出 libjitgcd.a，再编译 demo
add_dependencies(jit_staticlibrary_demo jitgcd)
