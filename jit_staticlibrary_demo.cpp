/**
 * @file    jit_staticlibrary_demo.cpp
 * @brief   LLVM ORC JIT 调用"静态库"中实现的函数
 *
 * 场景：我们用 LLVM C++ API 生成一段 IR，IR 里会调用 gcd()，
 * 但 gcd() 的实现不在 IR 里、也不在主程序里，而是在一个静态库
 * （.a 归档文件，由 jit_demo.cpp 编译成的 libjitgcd.a）中。
 *
 * 关键机制：ORC 的 DefinitionGenerator。
 *   StaticLibraryDefinitionGenerator 把一个 .a 文件当作"符号来源"
 *   挂到 JITDylib 上。JIT 链接时发现未定义符号，就会询问这个
 *   generator，后者像静态链接器一样：按需把提供该符号的目标文件
 *   成员从归档中取出，JIT 编译进内存（只拉取用到的成员，成员之间
 *   的相互引用也会被递归解析）。
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

// ----- 静态库支持（本例的核心）-----
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
// StaticLibraryDefinitionGenerator: 让 JIT 能从 .a 归档中解析符号

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
    // LLJIT 是 ORC API 的高层封装，内部自动管理：
    //   - 代码生成器（IR → 机器码）
    //   - 内存管理器（分配可执行内存）
    //   - 链接层（解析符号引用，本例的主角）
    auto J = ExitOnErr(llvm::orc::LLJITBuilder().create());

    // ------------------------------------------------------------------
    //  Step 3: 【核心】把静态库挂到 JIT 的符号搜索路径上
    // ------------------------------------------------------------------
    // StaticLibraryDefinitionGenerator::Load 的两个参数：
    //   1. 对象链接层（archive 成员被取出来后，交给它重定位/加载）
    //   2. .a 归档文件的路径
    //
    // Load 会校验归档与 JIT 目标平台（triple）是否兼容。
    //
    // JITGCD_ARCHIVE_PATH 由 CMake 注入，指向 libjitgcd.a。
    //
    // 注意：主程序【并没有】链接 libjitgcd.a，进程地址空间里不存在
    // gcd 的实现——等会儿符号能解析成功，完全靠下面这个 generator。
    auto LibGen = ExitOnErr(
        llvm::orc::StaticLibraryDefinitionGenerator::Load(
            J->getObjLinkingLayer(), JITGCD_ARCHIVE_PATH));

    // 挂到 main JITDylib 上。此后 JIT 模块里的未定义符号，
    // 都会先问这个 generator："归档里有没有它的实现？"
    J->getMainJITDylib().addGenerator(std::move(LibGen));

    // ------------------------------------------------------------------
    //  Step 4: 构造 IR —— 声明外部 gcd，定义一个调用它的包装函数
    // ------------------------------------------------------------------
    // 等价的 C 代码：
    //
    //   extern int gcd(int x, int y);           // 实现在静态库里
    //
    //   int jit_gcd_plus(int x, int y) {
    //     return gcd(x, y) + 100;
    //   }
    //
    // ThreadSafeModule 的经典构造方式：
    //   先用一个独立的 LLVMContext 建 Module，最后一起 move 给 JIT。
    auto Context = std::make_unique<llvm::LLVMContext>();
    auto ModulePtr = std::make_unique<llvm::Module>("jit_staticlib_demo", *Context);

    auto *Int32Ty = llvm::Type::getInt32Ty(*Context);

    // ----- 4a: 声明静态库里的 gcd -----
    //
    // 【重要】这里的名字必须是目标文件里的"链接符号名"（link name）。
    // jit_demo.cpp 里 gcd 声明为 extern "C"，不做 C++ 名字修饰，
    // 所以归档中的符号名就是 "gcd"（可以用 nm libjitgcd.a 验证）。
    // 如果没有 extern "C"，按 C++ 编译后会被修饰成 "_Z3gcdii"，
    // 那时这里就必须用修饰后的名字。
    auto *GcdTy = llvm::FunctionType::get(Int32Ty, {Int32Ty, Int32Ty}, false);
    auto *GcdFn = llvm::Function::Create(
        GcdTy, llvm::Function::ExternalLinkage, "gcd", ModulePtr.get());
    // 注意：这只是 declare（没有函数体），实现由静态库在 JIT 链接期提供。

    // ----- 4b: 定义包装函数 jit_gcd_plus -----
    //
    //   define i32 @jit_gcd_plus(i32 %x, i32 %y) {
    //   entry:
    //     %g = call i32 @gcd(i32 %x, i32 %y)
    //     %r = add i32 %g, 100
    //     ret i32 %r
    //   }
    auto *WrapTy = llvm::FunctionType::get(Int32Ty, {Int32Ty, Int32Ty}, false);
    auto *WrapFn = llvm::Function::Create(
        WrapTy, llvm::Function::ExternalLinkage, "jit_gcd_plus", ModulePtr.get());

    auto Args = WrapFn->args();
    llvm::Value *ArgX = Args.begin();
    llvm::Value *ArgY = Args.begin() + 1;

    auto *Entry = llvm::BasicBlock::Create(*Context, "entry", WrapFn);
    llvm::IRBuilder<> B(Entry);
    llvm::Value *G = B.CreateCall(GcdFn, {ArgX, ArgY}, "g");   // 调用静态库函数
    llvm::Value *R = B.CreateAdd(G, B.getInt32(100), "r");
    B.CreateRet(R);

    // ------------------------------------------------------------------
    //  Step 5: 打印并验证 IR
    // ------------------------------------------------------------------
    llvm::outs() << "; ========== 生成的 LLVM IR ==========\n";
    ModulePtr->print(llvm::outs(), nullptr);
    llvm::outs() << "; ====================================\n";

    llvm::verifyModule(*ModulePtr, &llvm::errs());

    // ------------------------------------------------------------------
    //  Step 6: 提交给 JIT 编译
    // ------------------------------------------------------------------
    // 此时 JIT 里的符号状态：
    //   jit_gcd_plus  —— 有定义（刚提交的 IR）
    //   gcd           —— 未定义，链接时会触发 Step 3 挂上的 generator，
    //                    从 libjitgcd.a 中把实现它的成员取出来编译
    ExitOnErr(J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(ModulePtr), std::move(Context))));

    // ------------------------------------------------------------------
    //  Step 7: 查找并调用
    // ------------------------------------------------------------------
    using BinFn = int (*)(int, int);
    auto GcdPlusFn = ExitOnErr(J->lookup("jit_gcd_plus")).toPtr<BinFn>();

    // gcd(1071, 462) = 21  →  21 + 100 = 121
    llvm::outs() << "jit_gcd_plus(1071, 462) = " << GcdPlusFn(1071, 462) << "\n";
    // gcd(17, 5) = 1  →  1 + 100 = 101
    llvm::outs() << "jit_gcd_plus(17, 5)     = " << GcdPlusFn(17, 5) << "\n";

    return 0;
}
