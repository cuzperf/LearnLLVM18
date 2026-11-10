/**
 * @file    call_demo.cpp
 * @brief   系统演示 LLVM 的 call 机制（IR call 指令 + IRBuilder::CreateCall API）
 *
 * 本 demo 在同一个模块里构造六种调用形式，交给 ORC JIT 编译执行：
 *
 *   1. 直接调用      call i32 @callee_double(i32 %x)
 *                      —— 被调方是本模块里有定义的函数
 *   2. 外部函数调用  call i32 (ptr, ...) @printf(ptr %fmt, i32 %v)
 *                      —— 被调方只有声明，实现在 libc，运行期从进程符号表解析
 *                      —— 同时演示变参（varargs）函数的声明与调用
 *   3. 间接调用      call i32 %fp(i32 %v)
 *                      —— 被调方是一个函数指针值（运行期才能确定指向谁）
 *   4. tail + fastcc tail call fastcc i32 @helper(i32 %x)
 *                      —— 尾调用标记 + 调用约定（calling convention）
 *   5. intrinsic     call i32 @llvm.ctpop.i32(i32 %x)
 *                      —— LLVM 内建函数（数置位个数），无外部依赖
 *   6. （注释说明）   invoke / musttail / callbr 等其他形式
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
#include <llvm/IR/Intrinsics.h>    // Intrinsic::getDeclaration(): 取内建函数声明

// ----- ORC JIT 引擎 -----
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

// ----- 进程符号表搜索（给 printf 用）-----
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
// DynamicLibrarySearchGenerator::GetForCurrentProcess: JIT 用 dlsym 在
// 当前进程（含已加载的 libc）里查符号

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
    //  Step 1: 初始化本机代码生成器 + LLJIT
    // ------------------------------------------------------------------
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    llvm::ExitOnError ExitOnErr;
    auto J = ExitOnErr(llvm::orc::LLJITBuilder().create());

    // printf 的实现不在我们的模块里、也不在本可执行文件里，而在 libc.so 中。
    // GetForCurrentProcess 让整个进程的动态符号表（含已加载的 libc）
    // 成为 JIT 的符号来源。
    J->getMainJITDylib().addGenerator(
        ExitOnErr(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            J->getDataLayout().getGlobalPrefix())));

    // ------------------------------------------------------------------
    //  Step 2: 创建模块与公共类型
    // ------------------------------------------------------------------
    auto Context = std::make_unique<llvm::LLVMContext>();
    auto M = std::make_unique<llvm::Module>("call_demo", *Context);

    auto *I32 = llvm::Type::getInt32Ty(*Context);
    auto *Ptr = llvm::PointerType::getUnqual(*Context);  // 不透明指针 ptr

    llvm::IRBuilder<> B(*Context);

    // 小工具：造一个 "单基本块 + 立即返回" 的函数骨架，返回插入点
    auto MakeFn = [&](const char *Name, llvm::FunctionType *FT) {
        auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, *M);
        B.SetInsertPoint(llvm::BasicBlock::Create(*Context, "entry", F));
        return F;
    };

    // ------------------------------------------------------------------
    //  Step 3: 【形式一】直接调用 —— 被调方是本模块内有定义的函数
    // ------------------------------------------------------------------
    // define i32 @callee_double(i32 %x) { ret i32 %x * 2 }
    auto *BinFT = llvm::FunctionType::get(I32, {I32}, false);
    auto *DoubleFn = MakeFn("callee_double", BinFT);
    B.CreateRet(B.CreateMul(DoubleFn->getArg(0), B.getInt32(2)));

    // define i32 @call_direct(i32 %x) { %r = call i32 @callee_double(i32 %x); ret %r }
    auto *DirectFn = MakeFn("call_direct", BinFT);
    B.CreateRet(B.CreateCall(DoubleFn, {DirectFn->getArg(0)}, "r"));

    // ------------------------------------------------------------------
    //  Step 4: 【形式二】外部函数 + 变参调用 —— printf
    // ------------------------------------------------------------------
    // declare i32 @printf(ptr, ...)
    // 第三个参数 isVarArg=true 是关键：声明"固定参数之后还可以跟任意个参数"。
    // 调用变参函数时，实参个数/类型由调用点决定，声明里只写固定部分。
    auto *PrintfTy = llvm::FunctionType::get(I32, {Ptr}, /*isVarArg=*/true);
    auto *PrintfFn = llvm::Function::Create(
        PrintfTy, llvm::Function::ExternalLinkage, "printf", *M);

    // define i32 @call_printf(i32 %v) {
    //   %fmt = ... "call_printf: %d\n"（全局字符串常量）
    //   %r = call i32 (ptr, ...) @printf(ptr %fmt, i32 %v)
    //   ret i32 %r   // printf 返回打印的字符数
    // }
    auto *CallPrintfFn = MakeFn("call_printf", BinFT);
    llvm::Value *Fmt = B.CreateGlobalStringPtr("call_printf: %d\n", "fmt");
    auto *CallPrintf = B.CreateCall(PrintfFn, {Fmt, CallPrintfFn->getArg(0)}, "r");
    // 注意 CreateCall 打印出的 IR 里，变参调用点会带完整签名：
    //   call i32 (ptr, ...) @printf(...)
    (void)CallPrintf;
    B.CreateRet(CallPrintf);

    // ------------------------------------------------------------------
    //  Step 5: 【形式三】间接调用 —— 被调方是一个函数指针值
    // ------------------------------------------------------------------
    // define i32 @square(i32 %x) { ret i32 %x * %x }
    auto *SquareFn = MakeFn("square", BinFT);
    B.CreateRet(B.CreateMul(SquareFn->getArg(0), SquareFn->getArg(0)));

    // define i32 @apply(ptr %fp, i32 %v) {
    //   %r = call i32 %fp(i32 %v)   // 调用 %fp 指向的函数
    //   ret i32 %r
    // }
    //
    // 间接调用时 IRBuilder 无法从指针值推出函数类型（不透明指针），
    // 所以要用 CreateCall(FunctionType*, Value*, Args) 这个重载，
    // 显式告知"按 i32(i32) 的签名来调用"。
    auto *ApplyTy = llvm::FunctionType::get(I32, {Ptr, I32}, false);
    auto *ApplyFn = MakeFn("apply", ApplyTy);
    auto *IndirectCall = B.CreateCall(BinFT, ApplyFn->getArg(0),
                                      {ApplyFn->getArg(1)}, "r");
    (void)IndirectCall;
    B.CreateRet(IndirectCall);

    // ------------------------------------------------------------------
    //  Step 6: 【形式四】tail call + fastcc 调用约定
    // ------------------------------------------------------------------
    // 调用约定（CallingConv）规定参数怎么传、栈谁清理：
    //   ccc（默认 C 约定）、fastcc（尽量全用寄存器）、coldcc 等。
    // 调用方与被调方的约定必须一致，否则行为未定义。
    //
    // tail 标记是优化提示：若调用后立即返回，可复用当前栈帧（尾调用消除）。
    // 更强的 musttail 则要求必须消除（否则报错），常用于 ABI 敏感的转发。
    //
    // define fastcc i32 @helper(i32 %x) { ret i32 %x + 1 }
    auto *HelperFn = MakeFn("helper", BinFT);
    HelperFn->setCallingConv(llvm::CallingConv::Fast);
    B.CreateRet(B.CreateAdd(HelperFn->getArg(0), B.getInt32(1)));

    // define i32 @call_fastcc(i32 %x) {
    //   %r = tail call fastcc i32 @helper(i32 %x)
    //   ret i32 %r
    // }
    auto *FastccFn = MakeFn("call_fastcc", BinFT);
    auto *TailCall = B.CreateCall(HelperFn, {FastccFn->getArg(0)}, "r");
    TailCall->setCallingConv(llvm::CallingConv::Fast);     // 与被调方一致
    TailCall->setTailCallKind(llvm::CallInst::TCK_Tail);   // tail 标记
    B.CreateRet(TailCall);

    // ------------------------------------------------------------------
    //  Step 7: 【形式五】调用 LLVM intrinsic —— llvm.ctpop.i32
    // ------------------------------------------------------------------
    // intrinsic 是 LLVM 内建函数：不需要外部实现，后端会把它 lowering 成
    // 目标机器的指令（x86-64 上是 POPCNT 或等价序列）。
    // Intrinsic::getDeclaration 按 ID + 类型参数取声明（ctpop 是多态的，
    // 对 i32/i64 等各有一份，所以要传类型）。
    //
    // define i32 @call_intrinsic(i32 %x) {
    //   %r = call i32 @llvm.ctpop.i32(i32 %x)
    //   ret i32 %r
    // }
    auto *CtpopFn = llvm::Intrinsic::getDeclaration(M.get(), llvm::Intrinsic::ctpop, {I32});
    auto *CallIntrFn = MakeFn("call_intrinsic", BinFT);
    B.CreateRet(B.CreateCall(CtpopFn, {CallIntrFn->getArg(0)}, "r"));

    // ------------------------------------------------------------------
    //  其他形式（本 demo 不展开，仅说明）：
    //
    //  invoke —— 带异常处理的调用：正常结果走 normal 目标块，抛异常时
    //            跳到 landingpad 块（C++ throw / setjmp 类场景的 IR 表达）
    //  musttail —— 比 tail 更强：要求编译器【必须】做尾调用消除，
    //            且对双方原型有严格限制
    //  callbr —— 调用后可能跳到多个后继之一，主要给内联汇编（如 Linux
    //            内核的 asm goto）使用
    //  call 指令还可挂属性：如参数上的 noundef/signext/byval/sret、
    //  调用点上的 nounwind/memory(...) 等（CallInst::addParamAttr 等）
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    //  Step 8: 打印 IR、验证、提交 JIT
    // ------------------------------------------------------------------
    llvm::outs() << "; ========== 生成的 LLVM IR ==========\n";
    M->print(llvm::outs(), nullptr);
    llvm::outs() << "; ====================================\n";

    llvm::verifyModule(*M, &llvm::errs());

    ExitOnErr(J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(M), std::move(Context))));

    // ------------------------------------------------------------------
    //  Step 9: 逐个查找并调用，验证六种形式
    // ------------------------------------------------------------------
    using BinFn = int (*)(int);

    // 形式一：直接调用  double(21) = 42
    auto Direct = ExitOnErr(J->lookup("call_direct")).toPtr<BinFn>();
    llvm::outs() << "call_direct(21) = " << Direct(21) << "\n";

    // 形式三：间接调用  apply(square, 7) = 49
    // 先查出 JIT 里 square 的地址，作为函数指针实参传给 apply
    auto SquareAddr = ExitOnErr(J->lookup("square"));
    using ApplyHostFn = int (*)(void *, int);
    auto Apply = ExitOnErr(J->lookup("apply")).toPtr<ApplyHostFn>();
    llvm::outs() << "call_indirect: apply(square, 7) = "
                 << Apply(reinterpret_cast<void *>(SquareAddr.getValue()), 7) << "\n";

    // 形式四：tail + fastcc  helper(41) = 42
    auto Fastcc = ExitOnErr(J->lookup("call_fastcc")).toPtr<BinFn>();
    llvm::outs() << "call_fastcc: tail+fastcc(41) = " << Fastcc(41) << "\n";

    // 形式五：intrinsic  ctpop(0xF0F0) = 8（0xF0F0 有 8 个置位）
    auto Intr = ExitOnErr(J->lookup("call_intrinsic")).toPtr<BinFn>();
    llvm::outs() << "call_intrinsic: ctpop(0xF0F0) = " << Intr(0xF0F0) << "\n";

    // 形式二：外部变参调用  printf("call_printf: %d\n", 123)
    // printf 自己打印一行，并返回打印的字符数（"call_printf: 123\n" = 17）
    auto CallPrintfHost = ExitOnErr(J->lookup("call_printf")).toPtr<BinFn>();
    llvm::outs() << "call_printf 返回值 = " << CallPrintfHost(123) << "\n";

    return 0;
}
