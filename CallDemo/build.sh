#!/usr/bin/env bash
#
# build.sh — CallDemo 构建与验证脚本
#
# 流程：
#   1. cmake 配置 + 编译 call_demo
#   2. 运行 call_demo：构造六种调用形式的 IR → JIT → 逐个执行
#   3. 自动核对输出中的关键结果行
#
# 用法：
#   ./build.sh          # Debug 构建 + 运行 + 验证
#   ./build.sh release  # Release 构建 + 运行 + 验证
#   ./build.sh debug    # Debug 构建 + 运行 + 验证
#   ./build.sh clean    # 清理构建目录
#
# 环境要求：
#   - cmake >= 3.20
#   - g++ 或 clang++（C++17）
#   - LLVM 19 开发包

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

# 默认编译器，可通过环境变量覆盖
CXX="${CXX:-g++}"

# 构建类型
BUILD_TYPE="${1:-debug}"
case "${BUILD_TYPE,,}" in
    release) BUILD_TYPE=Release ;;
    debug)   BUILD_TYPE=Debug   ;;
    clean)
        echo "==> 清理构建目录: ${BUILD_DIR}"
        rm -rf "${BUILD_DIR}"
        exit 0
        ;;
    *)       BUILD_TYPE=Debug   ;;
esac

echo "============================================"
echo " CallDemo 构建与验证脚本"
echo "============================================"
echo "  项目目录 : ${PROJECT_DIR}"
echo "  构建目录 : ${BUILD_DIR}"
echo "  构建类型 : ${BUILD_TYPE}"
echo "  编译器   : ${CXX}"
echo "============================================"

# ---------- 配置 ----------
echo ""
echo "==> cmake 配置阶段..."

cmake -S "${PROJECT_DIR}" \
      -B "${BUILD_DIR}" \
      -DCMAKE_CXX_COMPILER="${CXX}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

# ---------- 编译 ----------
echo ""
echo "==> cmake 编译阶段..."

cpu_count=$(nproc 2>/dev/null || echo 4)
cmake --build "${BUILD_DIR}" -j "${cpu_count}"

# ---------- 运行 ----------
echo ""
echo "==> 运行 call_demo..."

OUTPUT=$("${BUILD_DIR}/call_demo")
echo "${OUTPUT}"

# ---------- 验证 ----------
echo ""
echo "==> 核对关键结果行..."

FAIL=0
check() {
    if echo "${OUTPUT}" | grep -qF "$1"; then
        echo "    [通过] $1"
    else
        echo "    [失败] 期望看到: $1"
        FAIL=1
    fi
}

check "call i32 @callee_double"                 # 直接调用
check "call i32 (ptr, ...) @printf"             # 外部变参调用
check "call i32 %"                              # 间接调用（函数指针）
check "tail call fastcc i32 @helper"            # tail + fastcc
check "call i32 @llvm.ctpop.i32"                # intrinsic
check "call_direct(21) = 42"
check "call_indirect: apply(square, 7) = 49"
check "call_fastcc: tail+fastcc(41) = 42"
check "call_intrinsic: ctpop(0xF0F0) = 8"
check "call_printf: 123"                        # printf 自己打印的行
check "call_printf 返回值 = 17"                 # "call_printf: 123\n" 共 17 字符

if [[ "${FAIL}" -ne 0 ]]; then
    echo ""
    echo "==> 存在验证失败项"
    exit 1
fi

echo ""
echo "==> 构建、运行与验证全部完成"
