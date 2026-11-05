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

# ============================================================================
# 路线二 demo：源码 → LLVM IR → addIRModule
# ============================================================================

# 找 clang++（优先与 LLVM 同版本），用于构建期把源码编成 LLVM IR
find_program(CLANGXX_EXECUTABLE NAMES clang++-19 clang++-18 clang++ REQUIRED)

# 构建期把 jit_demo.cpp 编译成 LLVM IR 文本 gcd.ll
set(GCD_IR_FILE "${CMAKE_BINARY_DIR}/gcd.ll")
add_custom_command(
    OUTPUT "${GCD_IR_FILE}"
    COMMAND "${CLANGXX_EXECUTABLE}" -S -emit-llvm
            "${CMAKE_SOURCE_DIR}/jit_demo.cpp" -o "${GCD_IR_FILE}"
    DEPENDS "${CMAKE_SOURCE_DIR}/jit_demo.cpp"
    COMMENT "生成 LLVM IR: ${GCD_IR_FILE}"
    VERBATIM
)
add_custom_target(gcd_ir DEPENDS "${GCD_IR_FILE}")

add_executable(jit_irfile_demo jit_irfile_demo.cpp)
target_link_libraries(jit_irfile_demo PRIVATE LLVM)

# 宏定义：IR 文件的完整路径
target_compile_definitions(jit_irfile_demo PRIVATE
    GCD_IR_PATH="${GCD_IR_FILE}"
)

# 保证先生成 gcd.ll，再编译 demo
add_dependencies(jit_irfile_demo gcd_ir)

# ============================================================================
# 路线三 demo：源码 → 目标文件 .o → addObjectFile
# ============================================================================

# 构建期把 jit_demo.cpp 编译成可重定位目标文件 gcd_for_jit.o
# -fPIC：JIT 分配的可执行内存位置不固定，PIC 可避免重定位溢出
set(GCD_OBJ_FILE "${CMAKE_BINARY_DIR}/gcd_for_jit.o")
add_custom_command(
    OUTPUT "${GCD_OBJ_FILE}"
    COMMAND "${CMAKE_CXX_COMPILER}" -fPIC -c
            "${CMAKE_SOURCE_DIR}/jit_demo.cpp" -o "${GCD_OBJ_FILE}"
    DEPENDS "${CMAKE_SOURCE_DIR}/jit_demo.cpp"
    COMMENT "生成目标文件: ${GCD_OBJ_FILE}"
    VERBATIM
)
add_custom_target(gcd_obj DEPENDS "${GCD_OBJ_FILE}")

add_executable(jit_objfile_demo jit_objfile_demo.cpp)
target_link_libraries(jit_objfile_demo PRIVATE LLVM)

# 宏定义：目标文件的完整路径
target_compile_definitions(jit_objfile_demo PRIVATE
    GCD_OBJ_PATH="${GCD_OBJ_FILE}"
)

# 保证先生成 gcd_for_jit.o，再编译 demo
add_dependencies(jit_objfile_demo gcd_obj)

# ============================================================================
# 路线一 demo：函数实现编进主程序，JIT 从进程符号表解析
# ============================================================================

# 把 jit_demo.cpp 直接编进可执行文件：gcd 的实现就在进程地址空间里
add_executable(jit_hostsymbol_demo jit_hostsymbol_demo.cpp jit_demo.cpp)
target_link_libraries(jit_hostsymbol_demo PRIVATE LLVM)

# 关键：-rdynamic，把可执行文件的符号导出到动态符号表，
# 否则 GetForCurrentProcess（底层是 dlsym）查不到 gcd
set_target_properties(jit_hostsymbol_demo PROPERTIES
    ENABLE_EXPORTS ON
)

# ============================================================================
# 路线四 demo：函数实现在共享库 .so 中，JIT 运行期动态加载
# ============================================================================

# 把 jit_demo.cpp 编成共享库 libgcdshared.so
add_library(gcdshared SHARED jit_demo.cpp)

add_executable(jit_sharedlibrary_demo jit_sharedlibrary_demo.cpp)

# 只链接 LLVM，故意【不】链接 gcdshared：.so 由 JIT 在运行期 dlopen，
# 这样 demo 能跑通就证明符号确实是动态加载解析出来的
target_link_libraries(jit_sharedlibrary_demo PRIVATE LLVM)

# 宏定义：共享库的完整路径（$<TARGET_FILE:gcdshared> 展开为
# libgcdshared.so 的实际输出路径）
target_compile_definitions(jit_sharedlibrary_demo PRIVATE
    GCD_SO_PATH="$<TARGET_FILE:gcdshared>"
)

# 保证先产出 libgcdshared.so，再编译 demo
add_dependencies(jit_sharedlibrary_demo gcdshared)
