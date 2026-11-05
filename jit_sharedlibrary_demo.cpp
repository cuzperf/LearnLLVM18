/**
 * @file    jit_sharedlibrary_demo.cpp
 * @brief   路线四：函数实现在共享库 .so 中 → JIT 运行期动态加载
 *
 * 场景：我们用 LLVM C++ API 生成一段 IR，IR 里会调用 gcd()，
 * gcd() 的实现在 jit_demo.cpp 里，被编成了共享库 libgcdshared.so。
 * 处理方式：
 *   构建期：add_library(gcdshared SHARED jit_demo.cpp)
 *   运行期：DynamicLibrarySearchGenerator::Load 把 .so dlopen 进进程，
 *           并把其中的符号暴露给 JIT。
 *
 * 与路线三/静态库的区别：.a 和 .o 是把机器码【复制】进 JIT 内存
 * （重定位、加载都由 ORC 完成）；.so 则由系统的动态链接器 dlopen
 * 加载，JIT 只是通过 dlsym 查地址。因此 .so 天然是 PIC，也不存在
 * 重定位溢出的问题。
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

// ----- 共享库符号搜索（本例的核心）-----
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
// DynamicLibrarySearchGenerator::Load: dlopen 一个 .so 并暴露其符号

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
    //  Step 3: 【核心】加载共享库，把它的符号挂到 JIT 的搜索路径上
    // ------------------------------------------------------------------
    // DynamicLibrarySearchGenerator::Load 的两个必选参数：
    //   1. .so 文件路径（由 CMake 注入的 GCD_SO_PATH）
    //   2. 全局符号前缀（从 DataLayout 取，Linux 上为空字符）
    //
    // Load 会立即 dlopen 该库（永久加载，不会 dlclose），返回的
    // generator 之后用 dlsym 在其中查找 JIT 需要的符号。
    //
    // 注意：本程序【没有】链接 libgcdshared.so，库的加载完全发生
    // 在运行期、由下面这行触发。
    J->getMainJITDylib().addGenerator(
        ExitOnErr(llvm::orc::DynamicLibrarySearchGenerator::Load(
            GCD_SO_PATH, J->getDataLayout().getGlobalPrefix())));

    // ------------------------------------------------------------------
    //  Step 4: 构造调用方 IR —— 声明 gcd，定义一个调用它的包装函数
    // ------------------------------------------------------------------
    // 等价的 C 代码：
    //
    //   extern int gcd(int x, int y);           // 实现在 .so 里
    //
    //   int jit_gcd_plus(int x, int y) {
    //     return gcd(x, y) + 100;
    //   }
    //
    // jit_demo.cpp 里 gcd 声明为 extern "C"，不做名字修饰，
    // 所以 IR 里的声明直接用 "gcd"（可用 nm -D libgcdshared.so 验证）。
    auto Context = std::make_unique<llvm::LLVMContext>();
    auto ModulePtr = std::make_unique<llvm::Module>("jit_sharedlib_demo", *Context);

    auto *Int32Ty = llvm::Type::getInt32Ty(*Context);

    // declare i32 @gcd(i32, i32) —— 只有声明没有函数体，
    // 定义由 Step 3 加载的共享库提供
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
    // Step 3 加载的共享库里的实现上。
    using BinFn = int (*)(int, int);
    auto GcdPlusFn = ExitOnErr(J->lookup("jit_gcd_plus")).toPtr<BinFn>();

    // gcd(1071, 462) = 21  →  21 + 100 = 121
    llvm::outs() << "jit_gcd_plus(1071, 462) = " << GcdPlusFn(1071, 462) << "\n";
    // gcd(17, 5) = 1  →  1 + 100 = 101
    llvm::outs() << "jit_gcd_plus(17, 5)     = " << GcdPlusFn(17, 5) << "\n";

    return 0;
}
