/**
 * @file    jit_clanglib_demo.cpp
 * @brief   clang-as-a-library：进程内用 clang API 把源码编成 LLVM IR 再 JIT
 *
 * 对比 jit_runtimecompile_demo（运行期调 clang 可执行程序）：
 * 那里要 fork 一个 clang++ 子进程、写临时 .ll 文件、再读回来；
 * 本 demo 把 clang 当作库直接链接进程序（libclang-cpp），在同一个
 * 进程内完成"源码 → AST → LLVM IR"的全过程，产出 llvm::Module 后
 * 直接 addIRModule 给 JIT。没有子进程，没有临时文件。
 *
 * clang-repl（交互式 C++）用的就是这套机制（另加增量编译支持）。
 *
 * 流程：
 *   1. 构造 DiagnosticsEngine（编译诊断输出）
 *   2. CompilerInvocation::CreateFromArgs 从参数构造编译配置
 *      【注意】参数是 cc1 风格（ clang 前端的底层参数），
 *              不含 argv[0] 程序名
 *   3. CompilerInstance + EmitLLVMOnlyAction：编译源码，
 *      直接在我们提供的 LLVMContext 里产出 llvm::Module
 *   4. addIRModule 提交给 JIT，之后照旧构造调用方、lookup、执行
 *
 * 构建依赖：LLVM 18/19、libclang-19-dev（clang C++ 头文件 + libclang-cpp）
 */

// ============================================================================
//  头文件说明
// ============================================================================

// ----- 核心 IR -----
#include <llvm/IR/LLVMContext.h>   // LLVMContext: 管理类型、常量的生命周期
#include <llvm/IR/Module.h>        // Module:     IR 的最外层容器
#include <llvm/IR/IRBuilder.h>     // IRBuilder:  逐条生成 IR 指令的工厂
#include <llvm/IR/Verifier.h>      // verifyModule(): 检查 IR 合法性

// ----- ORC JIT 引擎 -----
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

// ----- clang 前端库（本例的核心）-----
#include <clang/Basic/Diagnostic.h>            // DiagnosticsEngine / DiagnosticIDs
#include <clang/Basic/DiagnosticOptions.h>     // DiagnosticOptions
#include <clang/Frontend/TextDiagnosticPrinter.h> // 把诊断打印到 stderr
#include <clang/Frontend/CompilerInstance.h>   // CompilerInstance: 一次编译的"大管家"
#include <clang/Frontend/CompilerInvocation.h> // CompilerInvocation: 编译参数配置
#include <clang/CodeGen/CodeGenAction.h>       // EmitLLVMOnlyAction: 只产出 LLVM IR

// ----- 杂项 -----
#include <llvm/Support/TargetSelect.h>   // 初始化本机代码生成器
#include <llvm/TargetParser/Host.h>     // sys::getDefaultTargetTriple()
#include <llvm/Support/Error.h>          // ExitOnError: 解包 Expected<T>
#include <llvm/Support/raw_ostream.h>    // outs()/errs(): LLVM 版 stdout/stderr

#include <memory>
#include <string>


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
    //  Step 3: 【核心】进程内编译 jit_demo.cpp → llvm::Module
    // ------------------------------------------------------------------

    // ----- 3a: 诊断引擎 -----
    // clang 编译过程中的警告/错误都通过 DiagnosticsEngine 报告。
    // TextDiagnosticPrinter 负责把它们打到 stderr（就像命令行 clang 那样）。
    // DiagnosticsEngine 默认会接管（delete）传入的 consumer。
    auto DiagOpts = llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions>(
        new clang::DiagnosticOptions());
    auto DiagIDs = llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs>(
        new clang::DiagnosticIDs());
    auto *DiagPrinter = new clang::TextDiagnosticPrinter(llvm::errs(), &*DiagOpts);
    clang::DiagnosticsEngine Diags(DiagIDs, DiagOpts, DiagPrinter);

    // ----- 3b: 从参数构造编译配置 -----
    //
    // 【重要】CreateFromArgs 期望的是 **cc1 风格**参数（clang 前端的底层
    // 参数集），不是驱动程序（clang++ 命令行）风格：
    //   - 不含 argv[0] 程序名
    //   - 只接受对 cc1 可见的选项（-std= / -x / -triple / -I 等都可见）
    //
    // 各参数含义：
    //   -triple        目标平台三元组，取本机默认值，保证与 JIT 一致
    //   -x c++         按 C++ 语言解析输入
    //   -std=c++17     语言标准
    //   -resource-dir  clang 内置头文件目录（stddef.h / stdarg.h 等），
    //                  由 CMake 通过 `clang++ -print-resource-dir` 注入；
    //                  jit_demo.cpp 没有任何 #include，这里只是演示。
    //                  若源码引用了 C/C++ 标准库头，还需用 -I 等参数
    //                  补齐头文件搜索路径。
    //   最后一个参数   输入源文件路径（位置参数）
    std::string Triple = llvm::sys::getDefaultTargetTriple();
    auto Inv = std::make_shared<clang::CompilerInvocation>();
    const char *ClangArgs[] = {
        "-triple", Triple.c_str(),
        "-x", "c++",
        "-std=c++17",
        "-resource-dir", CLANG_RESOURCE_DIR,
        JIT_DEMO_SOURCE_PATH,
    };
    if (!clang::CompilerInvocation::CreateFromArgs(*Inv, ClangArgs, Diags)) {
        llvm::errs() << "clang 参数解析失败\n";
        return 1;
    }

    // ----- 3c: 执行编译动作，直接产出 llvm::Module -----
    //
    // EmitLLVMOnlyAction = 词法/语法/语义分析 + 代码生成到 LLVM IR，
    // 但不生成机器码。构造时传入我们的 LLVMContext，产出的 Module
    // 就活在这个 Context 里，之后连同 Context 一起 move 给 JIT。
    //
    // ExecuteAction 内部会自动创建 Target / FileManager / SourceManager /
    // Preprocessor，无需手工准备。
    auto GcdContext = std::make_unique<llvm::LLVMContext>();

    clang::CompilerInstance CI;
    CI.setInvocation(std::move(Inv));
    CI.createDiagnostics();

    clang::EmitLLVMOnlyAction Act(GcdContext.get());
    if (!CI.ExecuteAction(Act)) {
        llvm::errs() << "clang 编译失败: " << JIT_DEMO_SOURCE_PATH << "\n";
        return 1;
    }
    std::unique_ptr<llvm::Module> GcdModule = Act.takeModule();
    if (!GcdModule) {
        llvm::errs() << "未能从 clang 取回编译产物（Module）\n";
        return 1;
    }

    // ----- 3d: 提交实现模块给 JIT -----
    ExitOnErr(J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(GcdModule), std::move(GcdContext))));

    // ------------------------------------------------------------------
    //  Step 4: 构造调用方 IR —— 声明 gcd，定义一个调用它的包装函数
    // ------------------------------------------------------------------
    // 等价的 C 代码：
    //
    //   extern int gcd(int x, int y);           // 实现在进程内编译的模块里
    //
    //   int jit_gcd_plus(int x, int y) {
    //     return gcd(x, y) + 100;
    //   }
    auto Context = std::make_unique<llvm::LLVMContext>();
    auto ModulePtr = std::make_unique<llvm::Module>("jit_clanglib_demo", *Context);

    auto *Int32Ty = llvm::Type::getInt32Ty(*Context);

    // declare i32 @gcd(i32, i32) —— 只有声明没有函数体，
    // 定义由 Step 3 进程内编译出的模块提供
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
    using BinFn = int (*)(int, int);
    auto GcdPlusFn = ExitOnErr(J->lookup("jit_gcd_plus")).toPtr<BinFn>();

    // gcd(1071, 462) = 21  →  21 + 100 = 121
    llvm::outs() << "jit_gcd_plus(1071, 462) = " << GcdPlusFn(1071, 462) << "\n";
    // gcd(17, 5) = 1  →  1 + 100 = 101
    llvm::outs() << "jit_gcd_plus(17, 5)     = " << GcdPlusFn(17, 5) << "\n";

    return 0;
}
