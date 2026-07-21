cmake_minimum_required(VERSION 3.20)

project(LearnLLVM LANGUAGES CXX)

# ============================================================================
# LLVM 配置
# ============================================================================
# 若未通过 -DLLVM_DIR 指定路径，则默认使用 llvm-18 的安装位置
if(NOT DEFINED LLVM_DIR)
    # 尝试 llvm-config-18 提供的 cmake 目录
    set(LLVM_DIR /usr/lib/llvm-18/lib/cmake/llvm)
endif()

# 查找 LLVM（CONFIG 模式，即 llvm-config.cmake 方式）
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# ============================================================================
# 全局编译设置
# ============================================================================
# 引入 LLVM 的头文件路径与宏定义（如 -std=c++17 等）
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ============================================================================
# 链接 LLVM 组件
# ============================================================================
# llvm_map_components_to_libnames 将逻辑组件名映射为实际的库名
# 需要的组件：
#   - Core          LLVM IR 核心（Type, Value, Module, Function 等）
#   - ExecutionEngine 传统执行引擎（备用）
#   - OrcJIT        ORC 即时编译器（LLJIT）
#   - MCJIT         MC 层 JIT（备用）
#   - Support       基础支持库（raw_ostream, Error 等）
#   - nativecodegen 本机代码生成器
llvm_map_components_to_libnames(llvm_libs
    Core
    ExecutionEngine
    OrcJIT
    OrcDebugging
    OrcTargetProcess
    MCJIT
    Support
    nativecodegen
)

# ============================================================================
# 构建目标
# ============================================================================
# 设置为 C++17（LLVM 18 要求的最低标准）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(jit_demo main.cpp)
target_link_libraries(jit_demo PRIVATE ${llvm_libs})

# ============================================================================
# 导出所有符号到动态符号表（JIT 调试必需）
# ============================================================================
# enableDebuggerSupport 内部通过 dlsym 查找
# llvm_orc_registerJITLoaderGDBWrapper 等注册函数，
# 需要用 -rdynamic 将所有符号导出到 .dynsym 段。
set_target_properties(jit_demo PROPERTIES
    ENABLE_EXPORTS ON
)

# ============================================================================
# 宏定义：项目源码根目录（用于 JIT 调试信息中定位源文件）
# ============================================================================
target_compile_definitions(jit_demo PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
