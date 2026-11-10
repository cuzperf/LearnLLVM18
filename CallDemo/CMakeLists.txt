cmake_minimum_required(VERSION 3.20)

# CallDemo —— 独立示例：LLVM call 机制（call 指令 + CreateCall API）
#
# 构建：
#   cmake -S CallDemo -B CallDemo/build
#   cmake --build CallDemo/build
#   ./CallDemo/build/call_demo
# 或一键：
#   ./CallDemo/build.sh

project(CallDemo LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# LLVM 配置（可用 -DLLVM_DIR=... 覆盖）
# ============================================================================
if(NOT DEFINED LLVM_DIR)
    set(LLVM_DIR /usr/lib/llvm-19/cmake)
endif()
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# 构建目标
# ============================================================================
add_executable(call_demo call_demo.cpp)

# 动态链接 libLLVM-19.so
target_link_libraries(call_demo PRIVATE LLVM)
