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
