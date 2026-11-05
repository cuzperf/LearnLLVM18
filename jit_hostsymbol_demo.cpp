/**
 * @file    jit_hostsymbol_demo.cpp
 * @brief   路线一：函数实现编进主程序 → JIT 从进程符号表解析
 *
 * 场景：我们用 LLVM C++ API 生成一段 IR，IR 里会调用 gcd()，
 * gcd() 的实现在 jit_demo.cpp 里，而 jit_demo.cpp 直接编进了
 * 本可执行文件。也就是说 gcd 的机器码本来就在进程地址空间里，
 * JIT 代码只要拿到它的地址就能调用。
 *
 * 处理方式：DynamicLibrarySearchGenerator::GetForCurrentProcess
 *   它把整个进程的动态符号表作为 JIT 的"符号来源"：JIT 链接时
 *   遇到未定义符号，就用 dlsym 在进程里查地址。
 *
 * 前提条件：符号必须出现在可执行文件的【动态】符号表中。
 * 普通编译时函数只进 .symtab，dlsym 查不到；需要链接时加
 * -rdynamic（本 demo 在 CMake 里用 ENABLE_EXPORTS ON 开启）。
 * 这正是 jit_demo 目标上同样设置的用途。
 *
 * 构建依赖：LLVM 18/19、CMake >= 3.20、C++17
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

// ----- 进程符号表搜索（本例的核心）-----
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
// DynamicLibrarySearchGenerator: 让 JIT 通过 dlsym 查找符号

// ----- 杂项 -----
#include <llvm/Support/TargetSelect.h>   // 初始化本机代码生成器
#include <llvm/Support/Error.h>          // ExitOnError: 解包 Expected<T>
#include <llvm/Support/raw_ostream.h>    // outs()/errs(): LLVM 版 stdout/stderr

#include <memory>

// gcd 的声明。定义在 jit_demo.cpp 里，与本文件一起编进可执行文件。
extern "C" int gcd(int x, int y);


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
    //  Step 3: 【核心】把进程自身的符号表挂到 JIT 的符号搜索路径上
    // ------------------------------------------------------------------
    // GetForCurrentProcess 的参数是全局符号前缀（GlobalPrefix）：
    // 某些平台（如 macOS）会给 C 符号加 "_" 前缀，从 DataLayout 取
    // 这个前缀可以保证跨平台正确。Linux 上它是空字符。
    //
    // 挂上之后，JIT 里的未定义符号会到本进程的动态符号表里查找，
    // gcd 就在其中（因为 jit_demo.cpp 编进了本程序，且启用了
    // ENABLE_EXPORTS = -rdynamic）。
    J->getMainJITDylib().addGenerator(
        ExitOnErr(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            J->getDataLayout().getGlobalPrefix())));

    // 备选方案：只暴露个别符号，而不是整张符号表。
    // 手动注册单个符号的写法（main.cpp 注册 GDB wrapper 用的就是它）：
    //   llvm::sys::DynamicLibrary::AddSymbol("gcd", (void*)&gcd);

    // 打印宿主进程里 gcd 的地址，直观说明"实现本来就在进程里"
    llvm::outs() << "宿主进程中的 gcd 地址: "
                 << reinterpret_cast<const void *>(&gcd) << "\n";

    // ------------------------------------------------------------------
    //  Step 4: 构造调用方 IR —— 声明 gcd，定义一个调用它的包装函数
    // ------------------------------------------------------------------
    // 等价的 C 代码：
    //
    //   extern int gcd(int x, int y);           // 实现在本程序里
    //
    //   int jit_gcd_plus(int x, int y) {
    //     return gcd(x, y) + 100;
    //   }
    auto Context = std::make_unique<llvm::LLVMContext>();
    auto ModulePtr = std::make_unique<llvm::Module>("jit_hostsymbol_demo", *Context);

    auto *Int32Ty = llvm::Type::getInt32Ty(*Context);

    // declare i32 @gcd(i32, i32) —— 只有声明没有函数体，
    // 定义由宿主进程的符号表提供
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
    // lookup 时 ORC 编译调用方 IR，把对 gcd 的调用链接到
    // Step 3 挂上的进程符号表里查到的地址上。
    using BinFn = int (*)(int, int);
    auto GcdPlusFn = ExitOnErr(J->lookup("jit_gcd_plus")).toPtr<BinFn>();

    // gcd(1071, 462) = 21  →  21 + 100 = 121
    llvm::outs() << "jit_gcd_plus(1071, 462) = " << GcdPlusFn(1071, 462) << "\n";
    // gcd(17, 5) = 1  →  1 + 100 = 101
    llvm::outs() << "jit_gcd_plus(17, 5)     = " << GcdPlusFn(17, 5) << "\n";

    return 0;
}
