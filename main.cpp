/**
 * @file    main.cpp
 * @brief   LLVM 18 JIT 编译与执行示例
 *
 * 本程序演示了如何使用 LLVM 18 的 ORC JIT API 完成以下步骤：
 *   1. 初始化 LLVM 环境（目标平台、上下文）
 *   2. 使用 IRBuilder 构造一个加法函数 add(int, int) -> int
 *   3. 将 IR Module 提交给 LLJIT 引擎进行即时编译
 *   4. 通过符号查找获取函数指针并调用执行
 *
 * 构建依赖：LLVM 18 (libLLVM-18)
 * 编译器要求：C++17（LLVM 18 要求 C++17）
 */

// ---------- LLVM 核心 IR 头文件 ----------
#include <llvm/IR/LLVMContext.h>   // LLVMContext: 线程安全的 LLVM 数据结构上下文
#include <llvm/IR/Module.h>        // Module: LLVM IR 的顶级容器，包含函数、全局变量等
#include <llvm/IR/IRBuilder.h>     // IRBuilder: 简化 IR 指令构造的工具类
#include <llvm/IR/Function.h>      // Function: 表示 IR 中的函数
#include <llvm/IR/Verifier.h>      // verifyModule/verifyFunction: 验证 IR 正确性

// ---------- ORC JIT 引擎头文件 ----------
// LLJIT 是 LLVM 18 推荐的 JIT 引擎，内部自动集成：
//   - 目标平台的代码生成（通过 nativecodegen）
//   - 内存管理（可执行内存的分配与释放）
//   - 符号解析（在同一 JIT 实例内的 Module 间查找符号）
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

// ---------- 目标平台与错误处理 ----------
#include <llvm/Support/TargetSelect.h>     // InitializeNativeTarget / InitializeNativeTargetAsmPrinter
#include <llvm/Support/Error.h>            // ExitOnError: 将 Expected<T> 转换为 T，失败时退出
#include <llvm/Support/raw_ostream.h>      // outs() / errs(): LLVM 的流式输出

#include <iostream>
#include <memory>

using namespace llvm;
using namespace llvm::orc;

int main()
{
    // ========================================================================
    // 第 1 步：初始化本地目标平台
    // ========================================================================
    // 这两步是必须的：告诉 LLVM 如何为本机 CPU 生成机器码。
    // InitializeNativeTarget          注册本机目标（x86/ARM 等）的 target machine
    // InitializeNativeTargetAsmPrinter 注册汇编打印器（JIT 需要将机器码写入内存）
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    // ========================================================================
    // 第 2 步：创建 LLVMContext（整个编译过程的核心上下文）
    // ========================================================================
    // LLVMContext 拥有所有 LLVM 类型和常量的生命周期。
    // 必须在创建任何 IR 对象（Type/Value/Module）之前创建。
    LLVMContext Context;

    // ========================================================================
    // 第 3 步：创建 Module（编译单元）
    // ========================================================================
    // Module 是 LLVM IR 的顶层容器，一个 Module 包含多个 Function 和 GlobalVariable。
    // 第一个参数 "jit_demo" 是模块标识名，通常用于调试信息。
    auto module = std::make_unique<Module>("jit_demo", Context);

    // ========================================================================
    // 第 4 步：定义函数类型 int add(int, int)
    // ========================================================================
    // FunctionType::get 的三个参数：
    //   - 返回值类型：Type::getInt32Ty(Context)    → int32
    //   - 参数类型列表：{int32, int32}             → 两个 int 参数
    //   - 是否为变参函数：false                    → 固定参数个数
    FunctionType *FT = FunctionType::get(
        Type::getInt32Ty(Context),           // 返回值：int32
        {Type::getInt32Ty(Context),
         Type::getInt32Ty(Context)},         // 参数：(int32, int32)
        false);                              // 非变参

    // 在 Module 中创建函数：
    //   - Function::ExternalLinkage 外部链接（其他编译单元可见）
    //   - "add" 是函数名，JIT 查找时用这个名字
    Function *AddFunc = Function::Create(FT, Function::ExternalLinkage, "add", module.get());

    // ========================================================================
    // 第 5 步：创建 BasicBlock 并填充函数体
    // ========================================================================
    // BasicBlock 是一组顺序执行的 IR 指令的容器。
    // 每个函数至少需要一个 BasicBlock（相当于 C 函数的花括号体）。
    BasicBlock *BB = BasicBlock::Create(Context, "entry", AddFunc);

    // IRBuilder 用于在 BasicBlock 中逐条插入 IR 指令。
    // 构造函数参数指定插入点：此处是 BB 的末尾。
    IRBuilder<> Builder(BB);

    // 获取函数参数：
    // AddFunc->args() 返回一个可迭代的参数列表。
    // 此处用迭代器依次取出两个参数 A 和 B。
    auto args = AddFunc->args();
    auto it = args.begin();
    Value *A = it++;   // 第一个参数
    Value *B = it;     // 第二个参数

    // 插入一条 add 指令：%sum = add i32 %A, %B
    Value *Sum = Builder.CreateAdd(A, B, "sum");

    // 插入一条 return 指令：ret i32 %sum
    Builder.CreateRet(Sum);

    // ========================================================================
    // 第 6 步：验证生成的 IR 是否正确
    // ========================================================================
    // verifyModule 检查 Module 的合法性（类型一致、SSA 正确、控制流合法等）。
    // 验证信息输出到 stderr（errs()）。
    // 注意：这里只做检查不阻塞，生产代码应检查返回值。
    verifyModule(*module, &errs());

    // ========================================================================
    // 第 7 步：创建 LLJIT 引擎实例
    // ========================================================================
    // ExitOnError 是一个辅助对象，将 Expected<T> 解包为 T；
    // 若 Expected 包含错误，则打印错误信息并调用 exit(1)。
    //
    // LLJIT 是 ORC API 的高级封装，内部自动：
    //   - 创建 JIT 编译器
    //   - 创建内存管理器
    //   - 管理动态链接层
    ExitOnError ExitOnErr;

    // LLJITBuilder().create() 返回 Expected<std::unique_ptr<LLJIT>>
    auto J = ExitOnErr(LLJITBuilder().create());

    // ========================================================================
    // 第 8 步：将 IR Module 提交给 JIT 编译
    // ========================================================================
    // ThreadSafeModule 是线程安全的 Module 包装器：
    //   - 第一个参数：拥有 IR 的 unique_ptr<Module>（所有权转移给 JIT）
    //   - 第二个参数：独立的 LLVMContext（确保跨线程使用的安全性）
    // 调用后 module 指针失效，不能再使用。
    ExitOnErr(J->addIRModule(
        ThreadSafeModule(std::move(module), std::make_unique<LLVMContext>())
    ));

    // ========================================================================
    // 第 9 步：查找已编译的函数符号
    // ========================================================================
    // J->lookup("add") 在 JIT 引擎中进行符号查找，返回 Expected<ExecutorAddr>
    auto Addr = ExitOnErr(J->lookup("add"));

    // 将 JIT 地址转换为可调用的函数指针
    using AddFn = int (*)(int, int);     // 定义函数指针类型
    auto *Add = Addr.toPtr<AddFn>();     // ExecutorAddr → 原生函数指针

    // ========================================================================
    // 第 10 步：调用 JIT 编译的函数
    // ========================================================================
    int result = Add(42, 58);
    outs() << "add(42, 58) = " << result << "\n";

    return 0;
}
