/**
 * @file    main.cpp
 * @brief   LLVM 18 JIT 编译与执行 — GCD（欧几里得算法）示例
 *
 * 本程序从零开始，用 LLVM C++ API 构造一个 gcd 函数的 IR（中间表示），
 * 然后通过 ORC JIT 引擎即时编译并执行。同时注入 DWARF 调试信息，
 * 使 GDB/VS Code 可以 F11 步入 JIT 生成的代码，并在真实源文件
 * jit_demo.cpp 上逐行跟踪。
 *
 * 构建依赖：LLVM 18、CMake >= 3.20、C++17 编译器
 */

// ============================================================================
//  头文件说明
// ============================================================================

// ----- 核心 IR（中间表示）-----
// LLVM IR 是编译器的"中间语言"，介于高级语言和机器码之间。
// 我们在这里用 C++ API 手工构造 IR，就像编译器前端做的那样。
#include <llvm/IR/LLVMContext.h>   // LLVMContext: 全局"上下文"，管理类型、常量的生命周期
#include <llvm/IR/Module.h>        // Module:     IR 的最外层容器，一个 Module = 一个编译单元
#include <llvm/IR/IRBuilder.h>     // IRBuilder:  工厂类，帮你逐条生成 IR 指令
#include <llvm/IR/Function.h>      // Function:   IR 中的函数
#include <llvm/IR/Verifier.h>      // verify*():  检查 IR 合法性（类型是否匹配、SSA 是否破坏等）

// ----- 调试信息（DWARF）-----
#include <llvm/IR/DIBuilder.h>     // DIBuilder:  为 IR 注入 DWARF 调试元数据（行号、变量名等）

// ----- ORC JIT 引擎 -----
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

// ----- GDB 调试器支持 -----
#include <llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/Support/DynamicLibrary.h>

// ----- 杂项 -----
#include <llvm/Support/TargetSelect.h>   // 初始化本机代码生成器
#include <llvm/Support/Error.h>          // ExitOnError: 把可能失败的返回值"解包"
#include <llvm/Support/raw_ostream.h>    // outs()/errs(): LLVM 版 stdout/stderr

#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>


// ============================================================================
//  main
// ============================================================================
int main()
{
    // ------------------------------------------------------------------
    //  Step 1: 初始化本机代码生成器
    // ------------------------------------------------------------------
    // LLVM 支持多种 CPU 架构（x86、ARM、RISC-V 等）。这两行告诉 LLVM
    // "请为当前这台机器的 CPU 准备好代码生成后端"。
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // ------------------------------------------------------------------
    //  Step 2: 创建 LLVMContext —— 整个编译过程的"大管家"
    // ------------------------------------------------------------------
    // LLVMContext 是整个 LLVM 的核心数据结构，负责管理：
    //   - 所有类型的唯一实例（所有的 i32 都是同一个对象）
    //   - 所有常量的唯一实例（所有的 0 都是同一个 ConstantInt）
    //   - 线程安全隔离（每个线程通常有自己的 Context）
    //
    // 必须在创建任何 IR 对象之前创建。
    llvm::LLVMContext Context;

    // ------------------------------------------------------------------
    //  Step 3: 创建 Module —— IR 的"文件"
    // ------------------------------------------------------------------
    // Module 是 IR 的顶层容器，对应 C 语言的一个 .c 文件。
    // 一个 Module 里面可以有多个 Function 和 GlobalVariable。
    // 第一个参数 "jit_demo" 是模块的标识名字，主要出现在调试输出中。
    auto module = std::make_unique<llvm::Module>("jit_demo", Context);

    // ------------------------------------------------------------------
    //  Step 4: 创建 DIBuilder —— 调试信息的"工厂"
    // ------------------------------------------------------------------
    // DIBuilder 负责生成 DWARF 调试元数据（行号映射、变量名、类型等），
    // 并把它们挂到 Module 上。GDB 读取这些元数据后，就能在调试时
    // 显示源码行号和变量值。
    llvm::DIBuilder DIB(*module);

    // === 调试用的基础类型定义 ===
    //
    // DIBasicType("int", 32, DW_ATE_signed) 的含义：
    //   名字 "int"  → GDB 把该类型显示为 int
    //   位宽 32     → 32 位整数
    //   DW_ATE_signed → 有符号编码
    //
    // 同时我们也需要 IR 层面的 i32 类型（Type::getInt32Ty），它们用途不同：
    //   IntTy   → 调试信息中描述变量类型（给 GDB 看）
    //   Int32Ty → 实际 IR 指令中操作的值类型（给 LLVM 看）
    auto *IntTy   = DIB.createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
    auto *Int32Ty = llvm::Type::getInt32Ty(Context);

    // === 源文件路径（调试信息用） ===
    //
    // DIFile 告诉 GDB "这段代码来自哪个文件"。
    // PROJECT_SOURCE_DIR 是 CMake 注入的宏，值为项目根目录的绝对路径。
    // 使用绝对路径确保 GDB 一定找得到 jit_demo.cpp。
    std::string GcdFileDir  = std::string(PROJECT_SOURCE_DIR);
    std::string GcdFilePath = GcdFileDir + "/jit_demo.cpp";
    auto *GcdFile = DIB.createFile(GcdFilePath, GcdFileDir);

    // === 编译单元（CU） ===
    //
    // DICompileUnit 对应一个编译单元，类似于 clang 的每个 .cpp 文件。
    // 即使我们只有一个 JIT 源文件，这也是 DWARF 规范要求的顶层结构。
    auto *CU = DIB.createCompileUnit(
        llvm::dwarf::DW_LANG_C_plus_plus,   // 语言：C++
        GcdFile,                       // 主源文件
        "LLVM JIT Demo",              // 生产者标识（类似于编译器名字）
        false,                         // isOptimized=false → 非优化构建，调试友好
        "",                            // 编译选项
        0);                            // 运行时版本

    // ============================================================================
    //  Step 5: 构造 jit_gcd 函数 —— int gcd(int x, int y)
    // ============================================================================
    //  以下代码完整实现 jit_demo.cpp 中 gcd 函数的 IR：
    //
    //    int gcd(int x, int y) {       // 第 1 行
    //      if (x < 0) x = -x;          // 第 2 行
    //      if (y < 0) y = -y;          // 第 3 行
    //      while (y) {                 // 第 4 行
    //        int z = x % y;            // 第 5 行
    //        x = y;                    // 第 6 行
    //        y = z;                    // 第 7 行
    //      }                           // 第 8 行
    //      return x;                   // 第 9 行
    //    }                             // 第 10 行
    //
    // ============================================================================

    // ----- 5a: 声明函数签名 -----
    //
    // FunctionType::get 的三个参数：
    //   1. 返回值类型  → i32
    //   2. 参数类型列表 → {i32, i32}
    //   3. 是否变参    → false（固定参数个数，不是 printf 那种）
    llvm::FunctionType *FT = llvm::FunctionType::get(Int32Ty, {Int32Ty, Int32Ty}, false);

    // 在 Module 中创建这个函数：
    //   ExternalLinkage → 外部链接（JIT 用 lookup 可以找到）
    //   "jit_gcd"       → 函数名，JIT 导入时用的名字，GDB 也以此识别
    llvm::Function *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "jit_gcd", module.get());

    // ----- 5b: 为该函数创建调试元数据 -----
    //
    // DISubroutineType: 描述函数签名（用于调试器显示函数原型）
    llvm::SmallVector<llvm::Metadata *, 3> EltTys;
    EltTys.push_back(IntTy);
    EltTys.push_back(IntTy);
    EltTys.push_back(IntTy);
    //                                                    ↑返回值  ↑参数1 ↑参数2
    auto *SubRoutineTy = DIB.createSubroutineType(DIB.getOrCreateTypeArray(EltTys));

    // DISubprogram: 描述一个函数/方法（即 Subprogram）
    //   参数依次为：所属 CU、函数名、链接名、源文件、定义行号、
    //             函数类型、作用域行号、标志位、子程序标志位
    auto *SP = DIB.createFunction(
        CU,                                        // 所属编译单元
        "jit_gcd",                                 // 函数名（GDB 显示名）
        "jit_gcd",                                 // 链接名（mangled name，这里和函数名一致）
        GcdFile,                                   // 源文件
        1,                                         // 定义所在行号（jit_demo.cpp 第 1 行）
        SubRoutineTy,                              // 函数类型
        1,                                         // 词法作用域起始行号
        llvm::DINode::FlagPrototyped,              // 有原型声明
        llvm::DISubprogram::SPFlagDefinition       // 这是定义（而非仅仅是声明）
    );
    F->setSubprogram(SP);  // 将调试元数据绑定到 IR 函数上

    // DILocalVariable: 描述函数参数（调试时 GDB 可显示参数名和值）
    auto *ParmX = DIB.createParameterVariable(
        SP, "x", 1, GcdFile, 1, IntTy, true, llvm::DINode::FlagZero);
    auto *ParmY = DIB.createParameterVariable(
        SP, "y", 2, GcdFile, 1, IntTy, true, llvm::DINode::FlagZero);
    //     ↑变量名 ↑参数序号 ↑文件 ↑行号 ↑类型 ↑保留参数（非可选） ↑无特殊标志

    // ----- 5c: 创建 BasicBlock（基本块）-----
    //
    // 和 C 语言的直觉不同，LLVM IR 的控制流用 BasicBlock 而非花括号组织。
    // BasicBlock 是一段"必须从头顺序执行到尾"的指令序列（最后一条是跳转）。
    //
    // 本例需要 8 个基本块，它们的职责分别是：
    auto *Entry     = llvm::BasicBlock::Create(Context, "entry",       F); // 入口：参数绑定和 alloc
    auto *IfThen1   = llvm::BasicBlock::Create(Context, "if.then1",    F); // if(x<0) 的 then 分支
    auto *IfEnd1    = llvm::BasicBlock::Create(Context, "if.end1",     F); // 第一个 if 的汇合点
    auto *IfThen2   = llvm::BasicBlock::Create(Context, "if.then2",    F); // if(y<0) 的 then 分支
    auto *IfEnd2    = llvm::BasicBlock::Create(Context, "if.end2",     F); // 第二个 if 的汇合点
    auto *WhileCond = llvm::BasicBlock::Create(Context, "while.cond",  F); // while 条件判断
    auto *WhileBody = llvm::BasicBlock::Create(Context, "while.body",  F); // while 循环体
    auto *WhileEnd  = llvm::BasicBlock::Create(Context, "while.end",   F); // while 退出后的代码

    // 一个常用的常数 0，后续比较会反复用到
    auto Z32 = llvm::ConstantInt::get(Int32Ty, 0);

    // ========================================================================
    //  下面是逐行生成 IR 指令的过程。
    //  我们使用 IRBuilder（下面简称 B）来插入指令。
    //  SetInsertPoint()     → 指定"下一条指令插到哪个基本块的末尾"
    //  SetCurrentDebugLocation() → 指定"下一条指令对应源码哪一行"
    // ========================================================================

    // ----- entry 块：函数入口 -----
    llvm::IRBuilder<> B(Entry);  // 构造 IRBuilder，初始插入点在 Entry 块的末尾

    // 获取函数的两个参数（在 IR 中，参数是 SSA 值，即寄存器）
    auto args = F->args();
    auto it = args.begin();
    llvm::Value *ArgX = it++;   // 第一个参数（寄存器）
    llvm::Value *ArgY = it;     // 第二个参数（寄存器）

    // 关键设计：为什么用 alloca？
    //
    // 在 LLVM IR 中，函数参数和指令结果都是 SSA 值（相当于只读寄存器），
    // 不能直接修改。但 gcd 算法需要重复修改 x 和 y。
    //
    // 解决方案：用 alloca（栈上分配）模拟 C 语言的局部变量。
    //   alloca → 分配栈空间 → 返回一个指针
    //   store  → 把值写入指针
    //   load   → 从指针读出值
    // 这样就能通过 load/store 实现"变量可变"的效果。
    // 这正是 clang -O0 生成代码的方式。
    auto *XAddr = B.CreateAlloca(Int32Ty, nullptr, "x");  // 等价于 int x;
    auto *YAddr = B.CreateAlloca(Int32Ty, nullptr, "y");  // 等价于 int y;
    auto *ZAddr = B.CreateAlloca(Int32Ty, nullptr, "z");  // 等价于 int z;
    B.CreateStore(ArgX, XAddr);  // x = 参数0;
    B.CreateStore(ArgY, YAddr);  // y = 参数1;

    // 调试信息：告诉 GDB "x 和 y 存在 XAddr/YAddr 指向的栈位置上"
    // insertDeclare 生成一个 llvm.dbg.declare 内置函数调用
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 1, 1, SP));
    DIB.insertDeclare(XAddr, ParmX, DIB.createExpression(),
                      llvm::DILocation::get(Context, 1, 1, SP), Entry);
    DIB.insertDeclare(YAddr, ParmY, DIB.createExpression(),
                      llvm::DILocation::get(Context, 1, 1, SP), Entry);

    // ----- 第 2 行：if (x < 0) x = -x; -----
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 2, 1, SP));
    //                                   行号=2 ↑列号=1 ↑所属函数 SP
    llvm::Value *X0   = B.CreateLoad(Int32Ty, XAddr);          // %t = load x
    llvm::Value *Cmp1 = B.CreateICmpSLT(X0, Z32);              // %t < 0 ? (有符号比较)
    B.CreateCondBr(Cmp1, IfThen1, IfEnd1);
    //               条件   为真去→  为假去→

    // --- IfThen1: x = -x ---
    B.SetInsertPoint(IfThen1);  // 从现在开始，指令插到 IfThen1 块
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 2, 1, SP));  // 仍在第 2 行
    llvm::Value *NegX = B.CreateNeg(B.CreateLoad(Int32Ty, XAddr));  // -x
    B.CreateStore(NegX, XAddr);                                // x = -x
    B.CreateBr(IfEnd1);  // 无条件跳转到 if.end1（汇合点）

    // --- IfEnd1: 第一个 if 之后 ---
    B.SetInsertPoint(IfEnd1);

    // ----- 第 3 行：if (y < 0) y = -y; -----
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 3, 1, SP));
    llvm::Value *Y0   = B.CreateLoad(Int32Ty, YAddr);
    llvm::Value *Cmp2 = B.CreateICmpSLT(Y0, Z32);
    B.CreateCondBr(Cmp2, IfThen2, IfEnd2);

    // --- IfThen2: y = -y ---
    B.SetInsertPoint(IfThen2);
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 3, 1, SP));
    llvm::Value *NegY = B.CreateNeg(B.CreateLoad(Int32Ty, YAddr));
    B.CreateStore(NegY, YAddr);
    B.CreateBr(IfEnd2);

    // --- IfEnd2: 进入 while 循环前 ---
    B.SetInsertPoint(IfEnd2);
    B.CreateBr(WhileCond);  // 无条件跳转到 while 条件判断

    // ----- 第 4 行：while (y) -----
    B.SetInsertPoint(WhileCond);
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 4, 1, SP));
    llvm::Value *YCond = B.CreateLoad(Int32Ty, YAddr);             // 加载 y 的当前值
    llvm::Value *Tobool = B.CreateIsNotNull(YCond);                 // y != 0 ?
    B.CreateCondBr(Tobool, WhileBody, WhileEnd);             // 真→循环体 假→退出

    // ----- 第 5 行：int z = x % y; -----
    B.SetInsertPoint(WhileBody);
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 5, 1, SP));
    llvm::Value *XBody = B.CreateLoad(Int32Ty, XAddr);
    llvm::Value *YBody = B.CreateLoad(Int32Ty, YAddr);
    llvm::Value *Rem   = B.CreateSRem(XBody, YBody);               // x % y (有符号取余)
    B.CreateStore(Rem, ZAddr);                                // z = x % y

    // ----- 第 6 行：x = y; -----
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 6, 1, SP));
    B.CreateStore(B.CreateLoad(Int32Ty, YAddr), XAddr);      // x = y

    // ----- 第 7 行：y = z; -----
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 7, 1, SP));
    B.CreateStore(B.CreateLoad(Int32Ty, ZAddr), YAddr);      // y = z

    // 循环体结束，跳回 while 条件判断处（形成循环）
    B.CreateBr(WhileCond);

    // ----- 第 9 行：return x; -----
    // 注意：行 8 是 while 的右花括号 }，通常不给它设置调试位置
    B.SetInsertPoint(WhileEnd);
    B.SetCurrentDebugLocation(llvm::DILocation::get(Context, 9, 1, SP));
    llvm::Value *RetVal = B.CreateLoad(Int32Ty, XAddr);   // 加载 x
    B.CreateRet(RetVal);                             // return x

    // ============================================================================
    //  Step 6: 完成调试信息，打印 IR 并验证
    // ============================================================================

    // finalize() 必须在使用 module 之前调用。
    // 它完成所有悬而未决的调试元数据，并给 CompileUnit 加上最终标记。
    DIB.finalize();

    // 把生成的 LLVM IR 文本打印到 stdout，可以直观看到刚才构造的结果
    llvm::outs() << "; ========== 生成的 LLVM IR ==========\n";
    module->print(llvm::outs(), nullptr);
    //                                    ↑ nullptr = 不额外输出调试元数据注释
    llvm::outs() << "; ====================================\n";

    // 验证 IR 合法性：类型是否一致、SSA 是否被破坏、控制流是否正确等。
    // 验证信息输出到 stderr。生产代码中应该检查返回值。
    llvm::verifyModule(*module, &llvm::errs());

    // ============================================================================
    //  Step 7: 创建 LLJIT 引擎并启用 GDB 调试
    // ============================================================================

    // ExitOnError: 辅助类。如果 Expected<T> 包含错误，就打印错误并 exit(1)。
    llvm::ExitOnError ExitOnErr;

    // LLJITBuilder().create() 创建一个 LLJIT 实例。
    // LLJIT 是 ORC API 的高层封装，内部自动管理：
    //   - 代码生成器（把 IR 编译成机器码）
    //   - 内存管理器（分配可执行内存）
    //   - 动态链接层（解析跨模块的符号引用）
    auto J = ExitOnErr(llvm::orc::LLJITBuilder().create());

    // enableDebuggerSupport 内部需要调用 llvm_orc_registerJITLoaderGDBWrapper 函数，
    // 但该函数在静态库 libLLVMOrcTargetProcess.a 中，不在动态符号表里。
    // DynamicLibrary::AddSymbol 手动把它注册到进程的全局符号表。
    llvm::sys::DynamicLibrary::AddSymbol(
        "llvm_orc_registerJITLoaderGDBWrapper",
        reinterpret_cast<void *>(&llvm_orc_registerJITLoaderGDBWrapper));

    // 【核心】启用 GDB 的 JIT 调试接口。
    // 之后每次 JIT 编译新代码时，LLVM 会自动通知 GDB "这里有新代码"，
    // 使得断点、单步等功能可以正常工作。
    ExitOnErr(llvm::orc::enableDebuggerSupport(*J));

    // ============================================================================
    //  Step 8: 把 Module 提交给 JIT 编译
    // ============================================================================

    // ThreadSafeModule 是 Module 的线程安全包装：
    //   第一个参数 → Module 的所有权转移给 JIT（此后不能再使用 module）
    //   第二个参数 → 独立的 LLVMContext，保证多线程安全
    ExitOnErr(J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(module), std::make_unique<llvm::LLVMContext>())
    ));

    // ============================================================================
    //  Step 9: 通过符号名查找已编译函数，并调用执行
    // ============================================================================

    // J->lookup("jit_gcd") 返回一个 Expected<ExecutorAddr>，
    // ExecutorAddr 是 JIT 生成的代码在内存中的地址。
    // .toPtr<FnType>() 把地址转换为可调用的 C 函数指针。
    using BinFn = int (*)(int, int);
    auto GcdFn = ExitOnErr(J->lookup("jit_gcd")).toPtr<BinFn>();

    // 就像调用普通 C 函数一样调用 JIT 生成的函数！
    // gcd(1071, 462) = 21（1071 和 462 的最大公约数是 21）
    llvm::outs() << "jit_gcd(1071, 462) = " << GcdFn(1071, 462) << "\n";

    return 0;
}
