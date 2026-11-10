/**
 * @file    jit_runtimecompile_demo.cpp
 * @brief   路线二的运行期版本：运行时才把 .cpp 源码编译成 LLVM IR 再 JIT
 *
 * 对比 jit_irfile_demo（路线二）：那里 gcd.ll 是【构建期】由 CMake
 * 调 clang++ 生成的；本 demo 把它挪到【运行期】——程序启动后自己
 * 调 clang++ 把 jit_demo.cpp 编成 IR，再 parseIRFile + addIRModule。
 *
 * 流程：
 *   1. 创建临时文件作为 IR 输出路径（/tmp/gcd_runtime-XXXXXX.ll）
 *   2. llvm::sys::ExecuteAndWait 调 clang++ -S -emit-llvm 编译源码
 *   3. parseIRFile 解析 IR，addIRModule 提交给 JIT
 *   4. 照常构造调用方 IR、lookup、执行
 *   5. 删除临时 IR 文件
 *
 * 说明：
 *   - 同理也可以让 clang 输出 .o 再 addObjectFile（路线三的运行期版本），
 *     机制完全相同，只是产物形式不同。
 *   - 生产级的"进程内编译"会直接把 clang 当库用（CompilerInstance API，
 *     clang-repl 就是这么做的），可以省去进程调用和临时文件开销；
 *     但调 clang 驱动程序是最简单、最直观的方式。
 *
 * 构建依赖：LLVM 18/19、clang（运行期调用）、CMake >= 3.20、C++17
 */

// ============================================================================
//  头文件说明
// ============================================================================

// ----- 核心 IR -----
#include <llvm/IR/LLVMContext.h>   // LLVMContext: 管理类型、常量的生命周期
#include <llvm/IR/Module.h>        // Module:     IR 的最外层容器
#include <llvm/IR/IRBuilder.h>     // IRBuilder:  逐条生成 IR 指令的工厂
#include <llvm/IR/Verifier.h>      // verifyModule(): 检查 IR 合法性

// ----- 从磁盘读取 LLVM IR -----
#include <llvm/IRReader/IRReader.h>  // parseIRFile(): 解析 .ll 或 .bc 文件
#include <llvm/Support/SourceMgr.h>  // SMDiagnostic: 解析失败时的诊断信息

// ----- ORC JIT 引擎 -----
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

// ----- 运行期编译（本例的核心）-----
#include <llvm/Support/Program.h>     // sys::ExecuteAndWait: 启动外部进程并等待
#include <llvm/Support/FileSystem.h>  // sys::fs::createTemporaryFile / remove

// ----- 杂项 -----
#include <llvm/Support/TargetSelect.h>   // 初始化本机代码生成器
#include <llvm/Support/Error.h>          // ExitOnError: 解包 Expected<T>
#include <llvm/Support/raw_ostream.h>    // outs()/errs(): LLVM 版 stdout/stderr

#include <memory>
#include <unistd.h>                      // close(): 关闭临时文件描述符


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
    //  Step 3: 【核心】运行期编译 —— 调 clang++ 把源码编成 LLVM IR
    // ------------------------------------------------------------------

    // ----- 3a: 申请一个临时文件路径作为 IR 输出 -----
    // createTemporaryFile 会在系统临时目录创建 /tmp/gcd_runtime-XXXXXX.ll
    // 并打开它，返回文件描述符和实际路径。我们只需要路径，所以立即关掉 fd，
    // 让 clang++ 自己去覆盖写这个文件。
    int TempFD = -1;
    llvm::SmallString<128> TempIRPath;
    if (auto EC = llvm::sys::fs::createTemporaryFile("gcd_runtime", "ll",
                                                     TempFD, TempIRPath)) {
        llvm::errs() << "创建临时文件失败: " << EC.message() << "\n";
        return 1;
    }
    close(TempFD);

    // ----- 3b: 调 clang++ 编译 jit_demo.cpp → 临时 IR 文件 -----
    // 等价命令行：clang++ -S -emit-llvm jit_demo.cpp -o /tmp/gcd_runtime-XXX.ll
    //
    // ExecuteAndWait 的第一个参数是要执行的程序，第二个参数是 argv 数组
    // （按惯例 argv[0] 是程序名自身）。CLANGXX_PATH 和
    // JIT_DEMO_SOURCE_PATH 均由 CMake 在配置期注入。
    llvm::outs() << "运行期编译: " << JIT_DEMO_SOURCE_PATH << "\n"
                 << "        → " << TempIRPath << "\n";
    llvm::SmallVector<llvm::StringRef, 8> Args = {
        CLANGXX_PATH, "-S", "-emit-llvm", JIT_DEMO_SOURCE_PATH, "-o", TempIRPath
    };
    if (int RC = llvm::sys::ExecuteAndWait(CLANGXX_PATH, Args); RC != 0) {
        llvm::errs() << "clang++ 编译失败，退出码: " << RC << "\n";
        llvm::sys::fs::remove(TempIRPath);
        return 1;
    }

    // ----- 3c: 解析 IR 并提交给 JIT -----
    // 从这里开始与 jit_irfile_demo（路线二）完全相同。
    auto GcdContext = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic ParseErr;
    auto GcdModule = llvm::parseIRFile(TempIRPath, ParseErr, *GcdContext);
    if (!GcdModule) {
        ParseErr.print("jit_runtimecompile_demo", llvm::errs());
        llvm::sys::fs::remove(TempIRPath);
        return 1;
    }

    // IR 已经解析进内存，临时文件没用了，删掉
    llvm::sys::fs::remove(TempIRPath);

    ExitOnErr(J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(GcdModule), std::move(GcdContext))));

    // ------------------------------------------------------------------
    //  Step 4: 构造调用方 IR —— 声明 gcd，定义一个调用它的包装函数
    // ------------------------------------------------------------------
    // 等价的 C 代码：
    //
    //   extern int gcd(int x, int y);           // 实现在运行期编译的模块里
    //
    //   int jit_gcd_plus(int x, int y) {
    //     return gcd(x, y) + 100;
    //   }
    auto Context = std::make_unique<llvm::LLVMContext>();
    auto ModulePtr = std::make_unique<llvm::Module>("jit_rtcompile_demo", *Context);

    auto *Int32Ty = llvm::Type::getInt32Ty(*Context);

    // declare i32 @gcd(i32, i32) —— 只有声明没有函数体，
    // 定义由 Step 3 运行期编译出的模块提供
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

    auto Args2 = WrapFn->args();
    llvm::Value *ArgX = Args2.begin();
    llvm::Value *ArgY = Args2.begin() + 1;

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
    using BinFn = int (*)(int, int);
    auto GcdPlusFn = ExitOnErr(J->lookup("jit_gcd_plus")).toPtr<BinFn>();

    // gcd(1071, 462) = 21  →  21 + 100 = 121
    llvm::outs() << "jit_gcd_plus(1071, 462) = " << GcdPlusFn(1071, 462) << "\n";
    // gcd(17, 5) = 1  →  1 + 100 = 101
    llvm::outs() << "jit_gcd_plus(17, 5)     = " << GcdPlusFn(17, 5) << "\n";

    return 0;
}
