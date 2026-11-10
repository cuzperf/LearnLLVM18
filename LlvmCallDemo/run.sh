#!/usr/bin/env bash
#
# run.sh — LlvmCallDemo 一键脚本（Julia llvmcall 演示）
#
# 流程：
#   1. 用 clang 把 payload.c 编成 LLVM bitcode（build/payload.bc），
#      供 llvmcall 形式三加载
#   2. 运行 llvmcall_demo.jl（内含 @testset 自检，失败会以非零退出）
#
# 用法：
#   ./run.sh          # 生成 bitcode + 运行演示
#   ./run.sh clean    # 清理构建目录
#
# 环境要求：
#   - julia（本机 1.13.3，内嵌 LLVM 20.1.8）
#   - clang（生成 bitcode；版本需 <= Julia 内嵌 LLVM 版本，否则读不了）

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BC_FILE="${BUILD_DIR}/payload.bc"

case "${1:-run}" in
    clean)
        echo "==> 清理构建目录: ${BUILD_DIR}"
        rm -rf "${BUILD_DIR}"
        exit 0
        ;;
esac

# 找 clang（用于生成 bitcode）
CLANG="${CLANG:-$(command -v clang-19 || command -v clang || true)}"
if [[ -z "${CLANG}" ]]; then
    echo "未找到 clang，无法生成 payload.bc" >&2
    exit 1
fi

echo "============================================"
echo " LlvmCallDemo 一键脚本（Julia llvmcall）"
echo "============================================"
echo "  项目目录 : ${PROJECT_DIR}"
echo "  clang    : ${CLANG}"
julia --banner=no -e 'println("  julia    : ", VERSION, "（内嵌 LLVM ", Base.libllvm_version, "）")'
echo "============================================"

# ---------- 生成 bitcode ----------
echo ""
echo "==> 生成 LLVM bitcode: ${BC_FILE}"
mkdir -p "${BUILD_DIR}"
"${CLANG}" -emit-llvm -c "${PROJECT_DIR}/payload.c" -o "${BC_FILE}"

# ---------- 运行演示（含自检）----------
echo ""
echo "==> 运行 llvmcall_demo.jl..."

julia --banner=no "${PROJECT_DIR}/llvmcall_demo.jl"

echo ""
echo "==> 演示与验证全部完成"
