/**
 * @file    main.cpp
 * @brief   LLVM 18 JIT 编译与执行示例（含 GDB 调试支持）
 *
 * 本程序演示了如何使用 LLVM 18 的 ORC JIT API 完成以下步骤：
 *   1. 初始化 LLVM 环境（目标平台、上下文）
 *   2. 使用 IRBuilder 构造一个加法函数 add(int, int) -> int
 *   3. 使用 DIBuilder 为生成的 IR 注入 DWARF 调试信息
 *   4. 通过 enableDebuggerSupport 注册 JIT 代码到 GDB
 *   5. 将 IR Module 提交给 LLJIT 引擎进行即时编译
 *   6. 通过符号查找获取函数指针并调用执行
 *
 * 构建依赖：LLVM 18 (libLLVM-18)
 * 编译器要求：C++17（LLVM 18 要求 C++17）
 *
 * GDB 调试：
 *   启动后程序会等待 GDB attach，然后编译并执行 JIT 函数。
 *   设置断点后可 F11 步入 JIT 生成的 add 函数。
 */

// ---------- LLVM 核心 IR 头文件 ----------
#include <llvm/IR/LLVMContext.h>       // LLVMContext: 线程安全的 LLVM 数据结构上下文
#include <llvm/IR/Module.h>             // Module: LLVM IR 的顶级容器，包含函数、全局变量等
#include <llvm/IR/IRBuilder.h>          // IRBuilder: 简化 IR 指令构造的工具类
#include <llvm/IR/Function.h>           // Function: 表示 IR 中的函数
#include <llvm/IR/Verifier.h>           // verifyModule/verifyFunction: 验证 IR 正确性

// ---------- 调试信息（DWARF）生成 ----------
#include <llvm/IR/DIBuilder.h>          // DIBuilder: 为 IR 注入 DWARF 调试信息

// ---------- ORC JIT 引擎头文件 ----------
// LLJIT 是 LLVM 18 推荐的 JIT 引擎，内部自动集成：
//   - 目标平台的代码生成（通过 nativecodegen）
//   - 内存管理（可执行内存的分配与释放）
//   - 符号解析（在同一 JIT 实例内的 Module 间查找符号）
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

// ---------- 调试器支持 ----------
// enableDebuggerSupport 将 JIT 编译的内存区域注册到 GDB，
// 使 GDB 能够识别、反汇编并步入 JIT 生成的代码。
#include <llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/Support/DynamicLibrary.h>

// ---------- 目标平台与错误处理 ----------
#include <llvm/Support/TargetSelect.h>  // InitializeNativeTarget / InitializeNativeTargetAsmPrinter
#include <llvm/Support/Error.h>         // ExitOnError: 将 Expected<T> 转换为 T，失败时退出
#include <llvm/Support/raw_ostream.h>   // outs() / errs(): LLVM 的流式输出

#include <iostream>
#include <memory>
#include <unistd.h>

using namespace llvm;
using namespace llvm::orc;

int main()
{
    // ========================================================================
    // 第 1 步：初始化本地目标平台
    // ========================================================================
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    // ========================================================================
    // 第 2 步：创建 LLVMContext 和 Module
    // ========================================================================
    LLVMContext Context;
    auto module = std::make_unique<Module>("jit_demo", Context);

    // ========================================================================
    // 第 3 步：定义函数签名 int add(int, int)
    // ========================================================================
    FunctionType *FT = FunctionType::get(
        Type::getInt32Ty(Context),
        {Type::getInt32Ty(Context),
         Type::getInt32Ty(Context)},
        false);                                        // 非变参

    Function *AddFunc = Function::Create(FT, Function::ExternalLinkage, "jit_add", module.get());

    // ========================================================================
    // 第 4 步：使用 DIBuilder 为 add 函数生成调试信息
    // ========================================================================
    DIBuilder DIB(*module);

    auto *File = DIB.createFile("jit_demo.cpp", ".");   // 源文件名（GDB 显示用）
    auto *CU = DIB.createCompileUnit(
        dwarf::DW_LANG_C_plus_plus,
        File,
        "LLVM JIT Demo",
        false,                                           // 非优化构建（便于调试）
        "", 0);

    // 调试用基本类型 "int"（32 位有符号整数）
    auto *IntTy = DIB.createBasicType("int", 32, dwarf::DW_ATE_signed);

    // 构造函数类型元数据数组：[返回值, 参数1, 参数2]
    SmallVector<Metadata *, 3> EltTys = {IntTy, IntTy, IntTy};
    auto *SubRoutineTy = DIB.createSubroutineType(DIB.getOrCreateTypeArray(EltTys));

    // 创建 DISubprogram —— 即 add 函数的调试信息描述
    auto *SP = DIB.createFunction(
        CU, "jit_add", "jit_add", File,
        1,                                  // 定义行号
        SubRoutineTy,
        1,                                  // 作用域行号
        DINode::FlagPrototyped,
        DISubprogram::SPFlagDefinition);
    AddFunc->setSubprogram(SP);

    // 为函数参数创建 DILocalVariable（GDB 中可显示参数名和类型）
    auto *ParmA = DIB.createParameterVariable(
        SP, "a", 1, File, 1, IntTy, true,
        DINode::FlagZero);
    auto *ParmB = DIB.createParameterVariable(
        SP, "b", 2, File, 1, IntTy, true,
        DINode::FlagZero);

    // ========================================================================
    // 第 5 步：填充函数体（含调试行号与参数绑定）
    // ========================================================================
    BasicBlock *BB = BasicBlock::Create(Context, "entry", AddFunc);
    IRBuilder<> Builder(BB);

    // 获取 IR 参数并为其关联调试信息变量
    auto args = AddFunc->args();
    auto it = args.begin();
    Value *A = it++;
    Value *B = it;

    // 在 entry 块起始处插入 dbg.value，将参数绑定到调试元数据
    // 注意：函数参数存放在寄存器中，应使用 insertDbgValueIntrinsic
    //        而非 insertDeclare（后者期望栈上 alloca）
    Builder.SetCurrentDebugLocation(DILocation::get(Context, 1, 1, SP));
    DIB.insertDbgValueIntrinsic(A, ParmA, DIB.createExpression(),
                                DILocation::get(Context, 1, 1, SP), BB);
    DIB.insertDbgValueIntrinsic(B, ParmB, DIB.createExpression(),
                                DILocation::get(Context, 1, 1, SP), BB);

    // 第 2 行：int sum = a + b;
    Builder.SetCurrentDebugLocation(DILocation::get(Context, 2, 1, SP));
    Value *Sum = Builder.CreateAdd(A, B, "sum");

    // 第 3 行：return sum;
    Builder.SetCurrentDebugLocation(DILocation::get(Context, 3, 1, SP));
    Builder.CreateRet(Sum);

    // 完成调试信息构建（必须在 Module 被消费前调用）
    DIB.finalize();

    // ========================================================================
    // 第 6 步：验证生成的 IR
    // ========================================================================
    verifyModule(*module, &errs());

    // ========================================================================
    // 第 7 步：创建 LLJIT 并启用 GDB 调试支持
    // ========================================================================
    ExitOnError ExitOnErr;
    auto J = ExitOnErr(LLJITBuilder().create());

    // 【关键】enableDebuggerSupport 内部通过 createJITLoaderGDBRegistrar
    // 查找 llvm_orc_registerJITLoaderGDBWrapper 符号。该符号在静态库
    // libLLVMOrcTargetProcess.a 中，不会自动出现在动态符号表中，
    // 因此需要手动注册到全局符号表，使 enableDebuggerSupport 能找到它。
    sys::DynamicLibrary::AddSymbol(
        "llvm_orc_registerJITLoaderGDBWrapper",
        reinterpret_cast<void *>(&llvm_orc_registerJITLoaderGDBWrapper));

    // enableDebuggerSupport 将 JIT 生成的代码注册到 GDB JIT 接口
    // (__jit_debug_register_code)，使 GDB 知道这些动态生成的代码区域。
    // ELF (Linux)  → 内部使用 DebugObjectManagerPlugin
    // MachO (macOS) → 内部使用 GDBJITDebugInfoRegistrationPlugin
    ExitOnErr(enableDebuggerSupport(*J));

    // ========================================================================
    // 第 8 步：将 IR Module 提交给 JIT 编译
    // ========================================================================
    ExitOnErr(J->addIRModule(
        ThreadSafeModule(std::move(module), std::make_unique<LLVMContext>())
    ));

    // ========================================================================
    // 第 9 步：查找已编译的函数符号并调用
    // ========================================================================
    auto Addr = ExitOnErr(J->lookup("jit_add"));
    using AddFn = int (*)(int, int);
    auto *Add = Addr.toPtr<AddFn>();

    int result = Add(42, 58);
    outs() << "add(42, 58) = " << result << "\n";

    return 0;
}
