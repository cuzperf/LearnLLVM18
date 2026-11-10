#!/usr/bin/env julia
#
# llvmcall_demo.jl —— Julia 的 llvmcall 使用演示
#
# 【llvmcall 是什么】
#   llvmcall 是 Julia 语言的内建函数（Core.llvmcall，日常用 Base.llvmcall），
#   【不是】 LLVM 项目的 API。它能存在，是因为 Julia 自己内嵌了 LLVM 作为
#   JIT 后端——llvmcall 把"解析一段 LLVM IR → 交给内嵌 LLVM 编译 → 直接调用"
#   封装成了一个语言级函数。
#
#   与本项目 C++ 部分的对应关系：
#     llvmcall 形式一 ≈ parseAssemblyString（函数体）+ LLJIT addIRModule + lookup
#     llvmcall 形式二 ≈ parseAssemblyString（完整模块）（主项目 jit_irfile_demo）
#     llvmcall 形式三 ≈ 解析 bitcode（jit_irfile_demo 里 parseIRFile 也能读 .bc）
#
# Julia 1.13 的三种调用形式（来自 Base.llvmcall 文档）：
#   1. llvmcall(函数体IR字符串, 返回类型, Tuple{参数类型...}, 实参...)
#   2. llvmcall((模块IR字符串, "入口函数名"), 返回类型, Tuple{...}, 实参...)
#   3. llvmcall((bitcode字节Vector{UInt8}, "入口函数名"), 返回类型, Tuple{...}, ...)
#
# 运行：./run.sh（会先编译 payload.bc，再运行本脚本）

using Test

println("Julia 版本: ", VERSION)
println("Julia 内嵌 LLVM 版本: ", Base.libllvm_version)
println()

# ============================================================================
# 形式一：函数体 IR 字符串
# ============================================================================
# Julia 会把这段 IR 包成一个函数定义再编译。约定：
#   - 参数依次是无名虚拟寄存器 %0, %1, ...
#   - 【坑】第一个无名临时变量必须从 %3 开始编号 —— Julia 的包装函数里
#     entry 基本块标签会占用 %2（IR 要求无名值连续编号，从 %2 开始会报
#     "instruction expected to be numbered %3"）。官方文档示例就是从 %3
#     写起的。
#   - 更省心的做法：像下面这样用【命名】寄存器（%r），与编号无关。

# --- 基础：单条指令 ---
add_i32(x, y) = Base.llvmcall("""
    %r = add i32 %0, %1
    ret i32 %r
    """, Int32, Tuple{Int32, Int32}, x, y)

# --- 进阶：多条指令 + 比较/选择（相当于 abs）---
abs_i32(x) = Base.llvmcall("""
    %neg = sub i32 0, %0
    %is_neg = icmp slt i32 %0, 0
    %r = select i1 %is_neg, i32 %neg, i32 %0
    ret i32 %r
    """, Int32, Tuple{Int32}, x)

# ============================================================================
# 形式二：(完整模块 IR, 入口函数名)
# ============================================================================
# 需要自己声明/定义函数的场景（调 intrinsic、调外部 C 函数）用这种形式。
# 模块里是标准 IR 规则：命名寄存器随意，无名值从 %0 连续编号。

# --- 调用 LLVM intrinsic（llvm.ctpop，数二进制置位个数）---
ctpop_i32(x) = Base.llvmcall(("""
declare i32 @llvm.ctpop.i32(i32)

define i32 @my_ctpop(i32 %x) {
    %r = call i32 @llvm.ctpop.i32(i32 %x)
    ret i32 %r
}
""", "my_ctpop"), Int32, Tuple{Int32}, x)

# --- 调用外部 C 函数（libc 的 strlen）---
# Julia 进程本身已加载 libc，IR 里 declare 的外部符号在 JIT 链接时
# 直接从进程符号表解析 —— 与 C++ demo 里 GetForCurrentProcess 同理。
const STRLEN_MOD = """
declare i64 @strlen(ptr)

define i64 @my_strlen(ptr %p) {
    %n = call i64 @strlen(ptr %p)
    ret i64 %n
}
"""
# 注意必须用 @eval + $ 插值：把 const 字符串变成字面量后传给 llvmcall
@eval my_strlen(s::String) = Base.llvmcall(
    ($STRLEN_MOD, "my_strlen"), Int64, Tuple{Ptr{UInt8}},
    Base.unsafe_convert(Ptr{UInt8}, s))

# ============================================================================
# 形式三：(bitcode 字节, 入口函数名)
# ============================================================================
# payload.c 由 clang 编成 payload.bc（见 run.sh），这里读字节直接调用。
#
# 两个注意点：
#   1. 【字面量要求】llvmcall 的 IR/字节码参数必须是字面量，不能是
#      read(...) 这样的表达式（否则报 "error statically evaluating llvm
#      IR argument"）。解决办法：先读进 const，再用 @eval 插值成字面量。
#   2. 【版本兼容】bitcode 向后兼容、向前不兼容。payload.bc 由 clang-19
#      生成，本机 Julia 内嵌 LLVM 20.1.8（更新），可以读取；反过来
#      （旧 LLVM 读新 bitcode）不行。

const BC_PATH = joinpath(@__DIR__, "build", "payload.bc")
const HAS_BITCODE = isfile(BC_PATH)

if HAS_BITCODE
    const PAYLOAD_BC = read(BC_PATH)
    @eval payload_triple(x::Int32) =
        Base.llvmcall(($PAYLOAD_BC, "triple"), Int32, Tuple{Int32}, x)
end

# ============================================================================
# 自检
# ============================================================================
println("========== 运行测试 ==========")
@testset "llvmcall 三种形式" begin

    println("\n[形式一] 函数体 IR 字符串")
    @test add_i32(Int32(41), Int32(1)) == 42
    println("  add_i32(41, 1) = ", add_i32(Int32(41), Int32(1)))
    @test abs_i32(Int32(-7)) == 7
    @test abs_i32(Int32(7)) == 7
    println("  abs_i32(-7) = ", abs_i32(Int32(-7)))

    println("\n[形式二] 模块 IR + 入口名（intrinsic / 外部 C 函数）")
    @test ctpop_i32(Int32(0xF0F0)) == 8
    println("  ctpop_i32(0xF0F0) = ", ctpop_i32(Int32(0xF0F0)))
    @test my_strlen("hello world") == 11
    println("  my_strlen(\"hello world\") = ", my_strlen("hello world"))

    println("\n[形式三] bitcode 字节 + 入口名（clang 编译的 payload.bc）")
    if HAS_BITCODE
        @test payload_triple(Int32(14)) == 42
        println("  payload_triple(14) = ", payload_triple(Int32(14)))
    else
        println("  （跳过：未找到 $(BC_PATH)，请先运行 ./run.sh）")
        @test true
    end
end

# ============================================================================
# 附：类型映射与注意事项（注释）
# ============================================================================
# Julia 类型 ↔ LLVM IR 类型：
#   Int8 ↔ i8     Int32 ↔ i32     Int64 ↔ i64
#   Float32 ↔ float   Float64 ↔ double
#   Ptr{T} ↔ ptr（不透明指针）   Bool ↔ i8（注意不是 i1）
#
# 注意事项：
#   1. 返回值只能是【单个】值；想返回多个结果可通过指针参数写出。
#   2. 参数类型必须写成元组类型 Tuple{Int32, ...}（不像 ccall 是类型元组）。
#   3. IR 文本、类型都必须是字面量，不能是变量或表达式（必要时 @eval）。
#   4. llvmcall ≠ ccall：ccall 调的是已编译好的 C ABI 函数；llvmcall 是
#      把 IR 源码/字节码交给 Julia 内嵌的 LLVM 现场编译后调用。
#   5. llvmcall 出来的函数首次调用时编译，之后与普通 Julia 函数一样快。

println("\n全部演示与测试完成")
