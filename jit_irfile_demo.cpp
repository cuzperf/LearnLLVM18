/**
 * @file    jit_irfile_demo.cpp
 * @brief   路线二：函数实现在 .c/.cpp 源码中 → 先编成 LLVM IR → addIRModule
 *
 * 场景：我们用 LLVM C++ API 生成一段 IR，IR 里会调用 gcd()，
 * gcd() 的实现在 jit_demo.cpp 里。处理方式：
 *   构建期：clang++ -S -emit-llvm jit_demo.cpp -o gcd.ll
 *   运行期：parseIRFile 把 gcd.ll 解析成 Module，同样 addIRModule
 *           提交给 JIT —— 相当于"连源码一起 JIT"。
 *
 * 关键点：ORC JIT 只认两种输入 —— LLVM IR 和可重定位目标文件。
 * 本 demo 演示第一种：把源码变成 IR。实现模块与调用方模块放进
 * 同一个 JITDylib，符号互相可见，链接（符号解析）延迟到 lookup 时
 * 才发生，所以两个 addIRModule 的先后顺序无所谓。
 *
 * 构建依赖：LLVM 18/19、clang（用于构建期生成 IR）、CMake >= 3.20
 */

// ============================================================================
//  头文件说明
// ============================================================================

// ----- 核心 IR -----
#include <llvm/IR/LLVMContext.h>   // LLVMContext: 管理类型、常量的生命周期
#include <llvm/IR/Module.h>        // Module:     IR 的最外层容器
#include <llvm/IR/IRBuilder.h>     // IRBuilder:  逐条生成 IR 指令的工厂
#include <llvm/IR/Verifier.h>      // verifyModule(): 检查 IR 合法性

// ----- 从磁盘读取 LLVM IR（本例的核心之一）-----
#include <llvm/IRReader/IRReader.h>  // parseIRFile(): 解析 .ll 或 .bc 文件
#include <llvm/Support/SourceMgr.h>  // SMDiagnostic: 解析失败时的诊断信息

// ----- ORC JIT 引擎 -----
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

// ----- 杂项 -----
#include <llvm/Support/TargetSelect.h>   // 初始化本机代码生成器
#include <llvm/Support/Error.h>          // ExitOnError: 解包 Expected<T>
#include <llvm/Support/raw_ostream.h>    // outs()/errs(): LLVM 版 stdout/stderr

#include <memory>


// ============================================================================
//  main
// ============================================================================
int main()
{
    // ------------------------------------------------------------------
    //  Step 1: 初始化本机代码生成器
    // ------------------------------------------------------------------
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // ExitOnError: 如果 Expected<T> 包含错误，打印错误并 exit(1)
    llvm::ExitOnError ExitOnErr;

    // ------------------------------------------------------------------
    //  Step 2: 创建 LLJIT 引擎
    // ------------------------------------------------------------------
    auto J = ExitOnErr(llvm::orc::LLJITBuilder().create());

    // ------------------------------------------------------------------
    //  Step 3: 【核心】把 gcd 的实现（LLVM IR 文件）提交给 JIT
    // ------------------------------------------------------------------
    // gcd.ll 是构建期用 clang++ 从 jit_demo.cpp 生成的 LLVM IR。
    // parseIRFile 既能读文本 IR（.ll），也能读二进制 bitcode（.bc）。
    //
    // 每个 Module 必须配一个独立的 LLVMContext（线程安全要求），
    // 这里为实现模块单独创建一个。
    auto GcdContext = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic ParseErr;
    auto GcdModule = llvm::parseIRFile(GCD_IR_PATH, ParseErr, *GcdContext);
    if (!GcdModule) {
        // 解析失败：打印诊断信息（含行号、原因）后退出
        ParseErr.print("jit_irfile_demo", llvm::errs());
        return 1;
    }

    // 提交实现模块。此后 JIT 里就有了 gcd 的定义。
    // GCD_IR_PATH 由 CMake 注入，指向构建目录下的 gcd.ll。
    ExitOnErr(J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(GcdModule), std::move(GcdContext))));

    // ------------------------------------------------------------------
    //  Step 4: 构造调用方 IR —— 声明 gcd，定义一个调用它的包装函数
    // ------------------------------------------------------------------
    // 等价的 C 代码：
    //
    //   extern int gcd(int x, int y);           // 实现在另一个 JIT 模块里
    //
    //   int jit_gcd_plus(int x, int y) {
    //     return gcd(x, y) + 100;
    //   }
    //
    // jit_demo.cpp 里 gcd 声明为 extern "C"，不做名字修饰，
    // 所以 IR 里的声明直接用 "gcd" 这个名字。
    auto Context = std::make_unique<llvm::LLVMContext>();
    auto ModulePtr = std::make_unique<llvm::Module>("jit_irfile_demo", *Context);

    auto *Int32Ty = llvm::Type::getInt32Ty(*Context);

    // declare i32 @gcd(i32, i32) —— 只有声明没有函数体，
    // 定义由 Step 3 提交的模块提供
    auto *GcdTy = llvm::FunctionType::get(Int32Ty, {Int32Ty, Int32Ty}, false);
    auto *GcdFn = llvm::Function::Create(
        GcdTy, llvm::Function::ExternalLinkage, "gcd", ModulePtr.get());

    // define i32 @jit_gcd_plus(i32 %x, i32 %y) {
    // entry:
    //   %g = call i32 @gcd(i32 %x, i32 %y)
    //   %r = add i32 %g, 100
    //   ret i32 %r
    // }
    auto *WrapTy = llvm::FunctionType::get(Int32Ty, {Int32Ty, Int32Ty}, false);
    auto *WrapFn = llvm::Function::Create(
        WrapTy, llvm::Function::ExternalLinkage, "jit_gcd_plus", ModulePtr.get());

    auto Args = WrapFn->args();
    llvm::Value *ArgX = Args.begin();
    llvm::Value *ArgY = Args.begin() + 1;

    auto *Entry = llvm::BasicBlock::Create(*Context, "entry", WrapFn);
    llvm::IRBuilder<> B(Entry);
    llvm::Value *G = B.CreateCall(GcdFn, {ArgX, ArgY}, "g");
    llvm::Value *R = B.CreateAdd(G, B.getInt32(100), "r");
    B.CreateRet(R);

    // ------------------------------------------------------------------
    //  Step 5: 打印并验证调用方 IR，然后提交给 JIT
    // ------------------------------------------------------------------
    llvm::outs() << "; ========== 生成的 LLVM IR ==========\n";
    ModulePtr->print(llvm::outs(), nullptr);
    llvm::outs() << "; ====================================\n";

    llvm::verifyModule(*ModulePtr, &llvm::errs());

    ExitOnErr(J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(ModulePtr), std::move(Context))));

    // ------------------------------------------------------------------
    //  Step 6: 查找并调用
    // ------------------------------------------------------------------
    // lookup 时 ORC 才真正编译、链接这两个模块：
    //   jit_gcd_plus  —— 定义在本文件生成的 IR 里
    //   gcd           —— 定义在 Step 3 提交的 gcd.ll 里
    using BinFn = int (*)(int, int);
    auto GcdPlusFn = ExitOnErr(J->lookup("jit_gcd_plus")).toPtr<BinFn>();

    // gcd(1071, 462) = 21  →  21 + 100 = 121
    llvm::outs() << "jit_gcd_plus(1071, 462) = " << GcdPlusFn(1071, 462) << "\n";
    // gcd(17, 5) = 1  →  1 + 100 = 101
    llvm::outs() << "jit_gcd_plus(17, 5)     = " << GcdPlusFn(17, 5) << "\n";

    return 0;
}
