/**
 * @file    main.cpp
 * @brief   LLVM 18 JIT 编译与执行示例（含 GDB 调试支持）
 *
 * 本程序演示了如何使用 LLVM 18 的 ORC JIT API：
 *   1. jit_add  — 简单加法函数，演示基本的 IR 生成与调试
 *   2. jit_gcd  — 欧几里得算法，调试信息映射到真实源文件 jit_demo.cpp
 *
 * 构建依赖：LLVM 18 (libLLVM-18)
 * 编译器要求：C++17
 */

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/DIBuilder.h>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace llvm;
using namespace llvm::orc;

int main()
{
    // ========================================================================
    // 环境初始化
    // ========================================================================
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    LLVMContext Context;
    auto module = std::make_unique<Module>("jit_demo", Context);
    DIBuilder DIB(*module);

    // 调试用基本类型
    auto *IntTy   = DIB.createBasicType("int", 32, dwarf::DW_ATE_signed);
    auto *Int32Ty = Type::getInt32Ty(Context);

    // ---- 用于 jit_add 的虚假源文件（行号仅供演示） ----
    auto *AddFile = DIB.createFile("jit_add.cpp", ".");
    auto *CU = DIB.createCompileUnit(
        dwarf::DW_LANG_C_plus_plus,
        AddFile, "LLVM JIT Demo",
        false, "", 0);

    // ---- 用于 jit_gcd 的真实源文件路径 ----
    // PROJECT_SOURCE_DIR 由 CMake 注入，确保 GDB 能定位到 jit_demo.cpp
    std::string GcdFilePath = std::string(PROJECT_SOURCE_DIR) + "/jit_demo.cpp";
    std::string GcdFileDir  = std::string(PROJECT_SOURCE_DIR);
    auto *GcdFile = DIB.createFile(GcdFilePath, GcdFileDir);

    // ========================================================================
    // 函数 1：jit_add —— int add(int a, int b)
    // ========================================================================
    {
        FunctionType *FT = FunctionType::get(Int32Ty, {Int32Ty, Int32Ty}, false);
        Function *F = Function::Create(FT, Function::ExternalLinkage, "jit_add", module.get());

        SmallVector<Metadata *, 3> EltTys = {IntTy, IntTy, IntTy};
        auto *SubRoutineTy = DIB.createSubroutineType(DIB.getOrCreateTypeArray(EltTys));
        auto *SP = DIB.createFunction(
            CU, "jit_add", "jit_add", AddFile,
            1, SubRoutineTy, 1,
            DINode::FlagPrototyped, DISubprogram::SPFlagDefinition);
        F->setSubprogram(SP);

        auto *ParmA = DIB.createParameterVariable(
            SP, "a", 1, AddFile, 1, IntTy, true, DINode::FlagZero);
        auto *ParmB = DIB.createParameterVariable(
            SP, "b", 2, AddFile, 1, IntTy, true, DINode::FlagZero);

        BasicBlock *BB = BasicBlock::Create(Context, "entry", F);
        IRBuilder<> Builder(BB);

        auto args = F->args();
        auto it = args.begin();
        Value *A = it++;
        Value *B = it;

        Builder.SetCurrentDebugLocation(DILocation::get(Context, 1, 1, SP));
        DIB.insertDbgValueIntrinsic(A, ParmA, DIB.createExpression(),
                                    DILocation::get(Context, 1, 1, SP), BB);
        DIB.insertDbgValueIntrinsic(B, ParmB, DIB.createExpression(),
                                    DILocation::get(Context, 1, 1, SP), BB);

        Builder.SetCurrentDebugLocation(DILocation::get(Context, 2, 1, SP));
        Value *Sum = Builder.CreateAdd(A, B, "sum");

        Builder.SetCurrentDebugLocation(DILocation::get(Context, 3, 1, SP));
        Builder.CreateRet(Sum);
    }

    // ========================================================================
    // 函数 2：jit_gcd —— 对应 jit_demo.cpp 中的 gcd 函数
    // ========================================================================
    {
        FunctionType *FT = FunctionType::get(Int32Ty, {Int32Ty, Int32Ty}, false);
        Function *F = Function::Create(FT, Function::ExternalLinkage, "jit_gcd", module.get());

        SmallVector<Metadata *, 3> EltTys = {IntTy, IntTy, IntTy};
        auto *SubRoutineTy = DIB.createSubroutineType(DIB.getOrCreateTypeArray(EltTys));

        // DSubprogram 指向真实 jit_demo.cpp，定义行号 = 1
        auto *SP = DIB.createFunction(
            CU, "jit_gcd", "jit_gcd", GcdFile,
            1, SubRoutineTy, 1,
            DINode::FlagPrototyped, DISubprogram::SPFlagDefinition);
        F->setSubprogram(SP);

        auto *ParmX = DIB.createParameterVariable(
            SP, "x", 1, GcdFile, 1, IntTy, true, DINode::FlagZero);
        auto *ParmY = DIB.createParameterVariable(
            SP, "y", 2, GcdFile, 1, IntTy, true, DINode::FlagZero);

        // ---- 创建所有 BasicBlock ----
        auto *Entry     = BasicBlock::Create(Context, "entry", F);
        auto *IfThen1   = BasicBlock::Create(Context, "if.then1", F);
        auto *IfEnd1    = BasicBlock::Create(Context, "if.end1", F);
        auto *IfThen2   = BasicBlock::Create(Context, "if.then2", F);
        auto *IfEnd2    = BasicBlock::Create(Context, "if.end2", F);
        auto *WhileCond = BasicBlock::Create(Context, "while.cond", F);
        auto *WhileBody = BasicBlock::Create(Context, "while.body", F);
        auto *WhileEnd  = BasicBlock::Create(Context, "while.end", F);

        auto Z32 = ConstantInt::get(Int32Ty, 0);

        // ---- entry: 参数绑定 + 局部变量 alloca ----
        IRBuilder<> B(Entry);
        auto args = F->args();
        auto it = args.begin();
        Value *ArgX = it++;
        Value *ArgY = it;

        // 使用 alloca 模拟栈变量（-O0 风格），这样 GDB 可以正确显示变量值
        auto *XAddr = B.CreateAlloca(Int32Ty, nullptr, "x");
        auto *YAddr = B.CreateAlloca(Int32Ty, nullptr, "y");
        auto *ZAddr = B.CreateAlloca(Int32Ty, nullptr, "z");
        B.CreateStore(ArgX, XAddr);
        B.CreateStore(ArgY, YAddr);

        // 参数调试信息
        B.SetCurrentDebugLocation(DILocation::get(Context, 1, 1, SP));
        DIB.insertDeclare(XAddr, ParmX, DIB.createExpression(),
                          DILocation::get(Context, 1, 1, SP), Entry);
        DIB.insertDeclare(YAddr, ParmY, DIB.createExpression(),
                          DILocation::get(Context, 1, 1, SP), Entry);

        // ---- Line 2: if (x < 0) x = -x; ----
        B.SetCurrentDebugLocation(DILocation::get(Context, 2, 1, SP));
        Value *X0 = B.CreateLoad(Int32Ty, XAddr);
        Value *Cmp1 = B.CreateICmpSLT(X0, Z32);
        B.CreateCondBr(Cmp1, IfThen1, IfEnd1);

        B.SetInsertPoint(IfThen1);
        // 仍然在第 2 行
        B.SetCurrentDebugLocation(DILocation::get(Context, 2, 1, SP));
        Value *NegX = B.CreateNeg(B.CreateLoad(Int32Ty, XAddr));
        B.CreateStore(NegX, XAddr);
        B.CreateBr(IfEnd1);

        // ---- Line 3: if (y < 0) y = -y; ----
        B.SetInsertPoint(IfEnd1);
        B.SetCurrentDebugLocation(DILocation::get(Context, 3, 1, SP));
        Value *Y0 = B.CreateLoad(Int32Ty, YAddr);
        Value *Cmp2 = B.CreateICmpSLT(Y0, Z32);
        B.CreateCondBr(Cmp2, IfThen2, IfEnd2);

        B.SetInsertPoint(IfThen2);
        // 仍然在第 3 行
        B.SetCurrentDebugLocation(DILocation::get(Context, 3, 1, SP));
        Value *NegY = B.CreateNeg(B.CreateLoad(Int32Ty, YAddr));
        B.CreateStore(NegY, YAddr);
        B.CreateBr(IfEnd2);

        // ---- 进入 while 循环 ----
        B.SetInsertPoint(IfEnd2);
        B.CreateBr(WhileCond);

        // ---- Line 4: while (y) ----
        B.SetInsertPoint(WhileCond);
        B.SetCurrentDebugLocation(DILocation::get(Context, 4, 1, SP));
        Value *YCond = B.CreateLoad(Int32Ty, YAddr);
        Value *Tobool = B.CreateIsNotNull(YCond);
        B.CreateCondBr(Tobool, WhileBody, WhileEnd);

        // ---- Line 5: int z = x % y; ----
        B.SetInsertPoint(WhileBody);
        B.SetCurrentDebugLocation(DILocation::get(Context, 5, 1, SP));
        Value *XBody = B.CreateLoad(Int32Ty, XAddr);
        Value *YBody = B.CreateLoad(Int32Ty, YAddr);
        Value *Rem = B.CreateSRem(XBody, YBody);
        B.CreateStore(Rem, ZAddr);

        // ---- Line 6: x = y; ----
        B.SetCurrentDebugLocation(DILocation::get(Context, 6, 1, SP));
        B.CreateStore(B.CreateLoad(Int32Ty, YAddr), XAddr);

        // ---- Line 7: y = z; ----
        B.SetCurrentDebugLocation(DILocation::get(Context, 7, 1, SP));
        B.CreateStore(B.CreateLoad(Int32Ty, ZAddr), YAddr);

        B.CreateBr(WhileCond);

        // ---- Line 9: return x; ----
        // 注意：跳过了 line 8（右花括号），直接到 line 9
        B.SetInsertPoint(WhileEnd);
        B.SetCurrentDebugLocation(DILocation::get(Context, 9, 1, SP));
        Value *RetVal = B.CreateLoad(Int32Ty, XAddr);
        B.CreateRet(RetVal);
    }

    // ========================================================================
    // 完成调试信息生成，打印 IR 并验证
    // ========================================================================
    DIB.finalize();

    // 将整个 Module 的 LLVM IR 输出到 stdout
    outs() << "; ========== 生成的 LLVM IR ==========\n";
    module->print(outs(), nullptr);
    outs() << "; ====================================\n";

    verifyModule(*module, &errs());

    // ========================================================================
    // 创建 LLJIT 并启用 GDB 调试支持
    // ========================================================================
    ExitOnError ExitOnErr;
    auto J = ExitOnErr(LLJITBuilder().create());

    sys::DynamicLibrary::AddSymbol(
        "llvm_orc_registerJITLoaderGDBWrapper",
        reinterpret_cast<void *>(&llvm_orc_registerJITLoaderGDBWrapper));

    ExitOnErr(enableDebuggerSupport(*J));

    // ========================================================================
    // 提交 Module 给 JIT
    // ========================================================================
    ExitOnErr(J->addIRModule(
        ThreadSafeModule(std::move(module), std::make_unique<LLVMContext>())
    ));

    // ========================================================================
    // 查找并调用 JIT 函数
    // ========================================================================
    using BinFn = int (*)(int, int);

    auto AddFn = ExitOnErr(J->lookup("jit_add")).toPtr<BinFn>();
    outs() << "jit_add(42, 58) = " << AddFn(42, 58) << "\n";

    auto GcdFn = ExitOnErr(J->lookup("jit_gcd")).toPtr<BinFn>();
    outs() << "jit_gcd(1071, 462) = " << GcdFn(1071, 462) << "\n";

    return 0;
}
