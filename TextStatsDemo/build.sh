#!/usr/bin/env bash
#
# build.sh — TextStatsDemo 构建与验证脚本
#
# 流程：
#   1. cmake 配置 + 编译（三种二进制产物 .o/.a/.so + make_libtextstats + test_loader）
#   2. 运行 make_libtextstats：clang-as-a-library 编译 textstats.c，
#      LLVM 后端出目标文件，LLD 链接生成 libtextstats.so
#   3. 自动验证产物：
#      - readelf 检查 DT_NEEDED / RUNPATH
#      - nm 检查导出符号（.o/.a 并入、.so 保持未定义）
#      - test_loader dlopen 运行并核对统计结果
#
# 用法：
#   ./build.sh          # Debug 构建 + 生成 + 验证
#   ./build.sh release  # Release 构建 + 生成 + 验证
#   ./build.sh debug    # Debug 构建 + 生成 + 验证
#   ./build.sh clean    # 清理构建目录
#
# 环境要求：
#   - cmake >= 3.20
#   - gcc（构建 .o/.a/.so 三种产物与 test_loader）
#   - llvm/clang/lld 19 开发包（libclang-19-dev、liblld-19-dev）

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
OUTPUT_SO="${BUILD_DIR}/libtextstats.so"

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
echo " TextStatsDemo 构建与验证脚本"
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

# ---------- 生成共享库 ----------
echo ""
echo "==> 生成 libtextstats.so（clang-as-a-library + LLVM 后端 + LLD）..."

"${BUILD_DIR}/make_libtextstats"

# ---------- 验证 ----------
echo ""
echo "==> 验证 1/3: 依赖关系（应包含 NEEDED libspaces.so 与 RUNPATH \$ORIGIN）"
readelf -d "${OUTPUT_SO}" | grep -E "NEEDED|RUNPATH"

echo ""
echo "==> 验证 2/3: 符号去向"
echo "    T = 已并入产物（.c 经 clang、.o、.a）  U = 未定义（留给 .so 运行时解析）"
nm -D "${OUTPUT_SO}" | grep ts_

echo ""
echo "==> 验证 3/3: dlopen 运行 test_loader"

# 核对统计结果（"hello world 42": 元音=3 辅音=7 数字=2 空格=2）
EXPECTED="元音=3 辅音=7 数字=2 空格=2"
ACTUAL=$("${BUILD_DIR}/test_loader" | tail -1)
echo "    输出: ${ACTUAL}"
if [[ "${ACTUAL}" == "${EXPECTED}" ]]; then
    echo "    结果核对: 通过"
else
    echo "    结果核对: 失败（期望: ${EXPECTED}）"
    exit 1
fi

echo ""
echo "==> 构建、生成与验证全部完成"
