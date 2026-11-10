cmake_minimum_required(VERSION 3.20)

# TextStatsDemo —— 独立示例：把 .c / .o / .a / .so 汇成一个共享库
#
#   textstats.c    ──(clang-as-a-library + LLVM 后端)──┐
#   consonants.c ──(gcc 构建期编成 .o)─────────────────┤
#   digits.c     ──(gcc 构建期编成 .a)─────────────────┼──(LLD)──> libtextstats.so
#   spaces.c     ──(gcc 构建期编成 .so)────────────────┘
#
# 构建：
#   cmake -S TextStatsDemo -B TextStatsDemo/build
#   cmake --build TextStatsDemo/build
#   ./TextStatsDemo/build/make_libtextstats     # 生成 libtextstats.so
#   ./TextStatsDemo/build/test_loader           # dlopen 验证

project(TextStatsDemo LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# LLVM / Clang / LLD 配置（均可用 -D 覆盖）
# ============================================================================
if(NOT DEFINED LLVM_DIR)
    set(LLVM_DIR /usr/lib/llvm-19/cmake)
endif()
find_package(LLVM REQUIRED CONFIG)

if(NOT DEFINED Clang_DIR)
    set(Clang_DIR /usr/lib/llvm-19/lib/cmake/clang)
endif()
find_package(Clang REQUIRED CONFIG)

if(NOT DEFINED LLD_DIR)
    set(LLD_DIR /usr/lib/llvm-19/lib/cmake/lld)
endif()
find_package(LLD REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# clang 内置头文件目录（演示 cc1 的 -resource-dir 参数）
find_program(CLANGXX_EXECUTABLE NAMES clang++-19 clang++-18 clang++ REQUIRED)
execute_process(
    COMMAND "${CLANGXX_EXECUTABLE}" -print-resource-dir
    OUTPUT_VARIABLE CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# ============================================================================
# 三种"预先存在"的二进制产物
# （故意由本项目的 C 编译器 gcc 构建：ELF 产物与编译器品牌无关）
# 全部 PIC：链接进共享库需要位置无关代码
# ============================================================================

# .o：OBJECT 库的产物就是一个目标文件
add_library(consonants_obj OBJECT consonants.c)
set_target_properties(consonants_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

# .a：静态库（链接时按需提取成员）
add_library(digits STATIC digits.c)
set_target_properties(digits PROPERTIES POSITION_INDEPENDENT_CODE ON)

# .so：共享库（最终成为 libtextstats.so 的 DT_NEEDED 依赖；
# CMake 默认会为共享库设置 SONAME=libspaces.so）
add_library(spaces SHARED spaces.c)

# ============================================================================
# 生成工具：clang-as-a-library 编译 .c + LLVM 后端出 .o + LLD 链接
# ============================================================================
add_executable(make_libtextstats make_libtextstats.cpp)

# clang-cpp = libclang-cpp 组合库；lldELF/lldCommon = LLD 静态组件
target_link_libraries(make_libtextstats PRIVATE clang-cpp lldELF lldCommon LLVM)

# 宏定义：四种输入产物与输出产物的路径
# $<TARGET_OBJECTS:...> / $<TARGET_FILE:...> 是 CMake 生成表达式，
# 展开为对应产物的实际输出路径
target_compile_definitions(make_libtextstats PRIVATE
    TEXTSTATS_C_PATH="${CMAKE_SOURCE_DIR}/textstats.c"
    CLANG_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
    CONSONANTS_OBJ_PATH="$<TARGET_OBJECTS:consonants_obj>"
    DIGITS_A_PATH="$<TARGET_FILE:digits>"
    SPACES_SO_PATH="$<TARGET_FILE:spaces>"
    OUTPUT_SO_PATH="${CMAKE_BINARY_DIR}/libtextstats.so"
)

# 保证三种二进制产物先就位，再编译生成工具
add_dependencies(make_libtextstats consonants_obj digits spaces)

# ============================================================================
# 验证程序：dlopen 生成的 libtextstats.so 并调用导出函数
# ============================================================================
add_executable(test_loader test_loader.c)
target_link_libraries(test_loader PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(test_loader PRIVATE
    TEXTSTATS_SO_PATH="${CMAKE_BINARY_DIR}/libtextstats.so"
)
