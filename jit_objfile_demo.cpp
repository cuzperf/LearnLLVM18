/**
 * @file    jit_objfile_demo.cpp
 * @brief   路线三：函数实现在 .c/.cpp 源码中 → 先编成目标文件 .o → addObjectFile
 *
 * 场景：我们用 LLVM C++ API 生成一段 IR，IR 里会调用 gcd()，
 * gcd() 的实现在 jit_demo.cpp 里。处理方式：
 *   构建期：g++ -fPIC -c jit_demo.cpp -o gcd_for_jit.o
 *   运行期：MemoryBuffer 读入 .o，LLJIT::addObjectFile 交给
 *           对象链接层（ObjectLinkingLayer）重定位、加载进 JIT 内存。
 *
 * 关键点：ORC JIT 只认两种输入 —— LLVM IR 和可重定位目标文件。
 * 本 demo 演示第二种：把源码变成 .o。上一个 jit_staticlibrary_demo
 * 的 .a 本质就是"一堆 .o 的打包"，本 demo 是它的"散装版"。
 *
 * 注意：.o 建议用 -fPIC 编译，JIT 分配的可执行内存位置不固定，
 * PIC 可避免小代码模型重定位溢出。
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

// ----- 从磁盘读入目标文件（本例的核心之一）-----
#include <llvm/Support/MemoryBuffer.h>  // MemoryBuffer::getFile(): 读文件进内存

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
    //  Step 3: 【核心】把 gcd 的实现（目标文件）提交给 JIT
    // ------------------------------------------------------------------
    // gcd_for_jit.o 是构建期用 C++ 编译器从 jit_demo.cpp 生成的。
    // addObjectFile 把这个 .o 交给 ObjectLinkingLayer：解析重定位、
    // 分配可执行内存、按链接语义把符号注册进 JITDylib。
    //
    // GCD_OBJ_PATH 由 CMake 注入，指向构建目录下的 gcd_for_jit.o。
    // 注意：MemoryBuffer::getFile 返回 ErrorOr（携带 std::error_code），
    // 不是 Expected，所以不能直接交给 ExitOnErr，这里手动检查。
    auto ObjBufferOrErr = llvm::MemoryBuffer::getFile(GCD_OBJ_PATH);
    if (!ObjBufferOrErr) {
        llvm::errs() << "无法读取目标文件 " << GCD_OBJ_PATH << ": "
                     << ObjBufferOrErr.getError().message() << "\n";
        return 1;
    }
    ExitOnErr(J->addObjectFile(std::move(*ObjBufferOrErr)));
    // 此后 JIT 里就有了 gcd 的定义（以机器码形式存在，
    // 与 addIRModule 的"先存 IR、用时再编译"不同，.o 里已是机器码）。

    // ------------------------------------------------------------------
    //  Step 4: 构造调用方 IR —— 声明 gcd，定义一个调用它的包装函数
    // ------------------------------------------------------------------
    // 等价的 C 代码：
    //
    //   extern int gcd(int x, int y);           // 实现在 .o 里
    //
    //   int jit_gcd_plus(int x, int y) {
    //     return gcd(x, y) + 100;
    //   }
    //
    // jit_demo.cpp 里 gcd 声明为 extern "C"，不做名字修饰，
    // 所以 IR 里的声明直接用 "gcd" 这个名字（可用 nm gcd_for_jit.o 验证）。
    auto Context = std::make_unique<llvm::LLVMContext>();
    auto ModulePtr = std::make_unique<llvm::Module>("jit_objfile_demo", *Context);

    auto *Int32Ty = llvm::Type::getInt32Ty(*Context);

    // declare i32 @gcd(i32, i32) —— 只有声明没有函数体，
    // 定义由 Step 3 提交的目标文件提供
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
    // lookup 时 ORC 编译调用方 IR，并把对 gcd 的调用链接到
    // Step 3 加载的目标文件里的实现上。
    using BinFn = int (*)(int, int);
    auto GcdPlusFn = ExitOnErr(J->lookup("jit_gcd_plus")).toPtr<BinFn>();

    // gcd(1071, 462) = 21  →  21 + 100 = 121
    llvm::outs() << "jit_gcd_plus(1071, 462) = " << GcdPlusFn(1071, 462) << "\n";
    // gcd(17, 5) = 1  →  1 + 100 = 101
    llvm::outs() << "jit_gcd_plus(17, 5)     = " << GcdPlusFn(17, 5) << "\n";

    return 0;
}
