#!/usr/bin/env bash
#
# build.sh — LLVM 18 JIT Demo 项目构建脚本
#
# 用法：
#   ./build.sh          # Debug 构建 + 运行
#   ./build.sh release  # Release 构建 + 运行
#   ./build.sh debug    # Debug 构建 + 运行
#   ./build.sh clean    # 清理构建目录
#
# 环境要求：
#   - cmake >= 3.20
#   - g++ 或 clang++ (支持 C++17)
#   - llvm-18 及对应的 llvm-config-18

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BINARY="${BUILD_DIR}/jit_demo"

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
echo " LLVM 18 JIT Demo 构建脚本"
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

# 使用所有 CPU 核心加速编译
cpu_count=$(nproc 2>/dev/null || echo 4)
cmake --build "${BUILD_DIR}" -j "${cpu_count}"

# ---------- 运行 ----------
echo ""
echo "==> 运行 JIT Demo..."

"${BINARY}"

echo ""
echo "==> 构建与运行完成"
