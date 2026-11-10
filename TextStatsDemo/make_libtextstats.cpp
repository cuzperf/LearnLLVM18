/**
 * @file    make_libtextstats.cpp
 * @brief   共享库生成工具：把 .c / .o / .a / .so 四种输入汇成一个 .so
 *
 * 输入：
 *   textstats.c    —— 运行期用 clang-as-a-library 编译（libclang-cpp）
 *   consonants.o   —— 预先存在的目标文件
 *   libdigits.a    —— 预先存在的静态库
 *   libspaces.so   —— 预先存在的共享库
 *
 * 处理流程（全在进程内完成，不 fork 任何外部工具）：
 *   Step 2  clang API：textstats.c → AST → LLVM IR（llvm::Module）
 *   Step 3  LLVM 后端（TargetMachine）：Module → 目标文件（内存 → 临时文件）
 *   Step 4  LLD-as-a-library（lld::elf::link）：
 *           把 clang 编出的 .o、consonants.o、libdigits.a、libspaces.so
 *           链接成 libtextstats.so
 *
 * 链接语义：
 *   .o / .a 的代码被【并入】产物（.a 按需提取成员）；
 *   .so 不并入，记录为 DT_NEEDED 运行时依赖；
 *   RUNPATH=$ORIGIN 让产物优先在同目录里找 libspaces.so。
 *
 * 构建依赖：LLVM 19、libclang-19-dev、liblld-19-dev
 */

// ============================================================================
//  头文件说明
// ============================================================================

// ----- clang 前端库：源码 → LLVM IR -----
#include <clang/Basic/Diagnostic.h>              // DiagnosticsEngine / DiagnosticIDs
#include <clang/Basic/DiagnosticOptions.h>       // DiagnosticOptions
#include <clang/Frontend/TextDiagnosticPrinter.h>// 诊断打印到 stderr
#include <clang/Frontend/CompilerInstance.h>     // CompilerInstance
#include <clang/Frontend/CompilerInvocation.h>   // CompilerInvocation::CreateFromArgs
#include <clang/CodeGen/CodeGenAction.h>         // EmitLLVMOnlyAction

// ----- LLVM 后端：IR → 目标文件 -----
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>    // legacy::PassManager（代码生成仍走旧 PM）
#include <llvm/MC/TargetRegistry.h>       // TargetRegistry::lookupTarget
#include <llvm/Target/TargetMachine.h>    // TargetMachine::addPassesToEmitFile
#include <llvm/Target/TargetOptions.h>

// ----- LLD 链接器库：目标文件们 → 共享库 -----
#include <lld/Common/Driver.h>

// 声明本可执行文件使用 lld 的 ELF 驱动（提供 lld::elf::link 的原型，
// 链接时需要对应库 lldELF / lldCommon）
LLD_HAS_DRIVER(elf)

// ----- 杂项 -----
#include <llvm/Support/TargetSelect.h>    // InitializeNativeTarget*
#include <llvm/TargetParser/Host.h>       // sys::getDefaultTargetTriple()
#include <llvm/Support/FileSystem.h>      // sys::fs::createTemporaryFile / remove
#include <llvm/Support/raw_ostream.h>     // outs()/errs()/raw_svector_ostream/raw_fd_ostream

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

    const std::string Triple = llvm::sys::getDefaultTargetTriple();

    // ------------------------------------------------------------------
    //  Step 2: clang-as-a-library —— textstats.c → llvm::Module
    // ------------------------------------------------------------------
    // 与 jit_clanglib_demo 相同的套路，两个差别：
    //   - 语言是 C（-x c -std=c11）
    //   - 加 -pic-level 2（等价于驱动的 -fPIC）：产物要链接进共享库，
    //     必须是位置无关代码

    // ----- 2a: 诊断引擎（编译警告/错误打到 stderr）-----
    auto DiagOpts = llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions>(
        new clang::DiagnosticOptions());
    auto DiagIDs = llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs>(
        new clang::DiagnosticIDs());
    auto *DiagPrinter = new clang::TextDiagnosticPrinter(llvm::errs(), &*DiagOpts);
    clang::DiagnosticsEngine Diags(DiagIDs, DiagOpts, DiagPrinter);

    // ----- 2b: 构造编译配置（cc1 风格参数，不含 argv[0]）-----
    auto Inv = std::make_shared<clang::CompilerInvocation>();
    const char *ClangArgs[] = {
        "-triple", Triple.c_str(),
        "-x", "c",
        "-std=c11",
        "-pic-level", "2",
        "-resource-dir", CLANG_RESOURCE_DIR,
        TEXTSTATS_C_PATH,
    };
    if (!clang::CompilerInvocation::CreateFromArgs(*Inv, ClangArgs, Diags)) {
        llvm::errs() << "clang 参数解析失败\n";
        return 1;
    }

    // ----- 2c: 执行 EmitLLVMOnlyAction，拿到 Module -----
    llvm::LLVMContext Ctx;
    clang::CompilerInstance CI;
    CI.setInvocation(std::move(Inv));
    CI.createDiagnostics();

    clang::EmitLLVMOnlyAction Act(&Ctx);
    if (!CI.ExecuteAction(Act)) {
        llvm::errs() << "clang 编译失败: " << TEXTSTATS_C_PATH << "\n";
        return 1;
    }
    std::unique_ptr<llvm::Module> M = Act.takeModule();
    if (!M) {
        llvm::errs() << "未能从 clang 取回编译产物（Module）\n";
        return 1;
    }
    llvm::outs() << "[1/3] clang 编译完成: " << TEXTSTATS_C_PATH << " → LLVM IR\n";

    // ------------------------------------------------------------------
    //  Step 3: LLVM 后端 —— Module → 目标文件（先输出到内存 buffer）
    // ------------------------------------------------------------------
    std::string LookupErr;
    const llvm::Target *T = llvm::TargetRegistry::lookupTarget(Triple, LookupErr);
    if (!T) {
        llvm::errs() << "未找到目标平台 " << Triple << ": " << LookupErr << "\n";
        return 1;
    }

    // Reloc::PIC_：与 Step 2 的 -pic-level 2 呼应，生成位置无关代码
    llvm::TargetOptions TOpt;
    std::unique_ptr<llvm::TargetMachine> TM(
        T->createTargetMachine(Triple, "generic", "", TOpt, llvm::Reloc::PIC_));

    llvm::SmallVector<char, 0> ObjData;
    {
        llvm::raw_svector_ostream ObjOS(ObjData);
        llvm::legacy::PassManager PM;
        if (TM->addPassesToEmitFile(PM, ObjOS, nullptr,
                                    llvm::CodeGenFileType::ObjectFile)) {
            llvm::errs() << "该目标不支持输出目标文件\n";
            return 1;
        }
        PM.run(*M);
    }

    // LLD 驱动按文件路径读取输入，把内存中的目标文件落到临时文件
    int ObjFD = -1;
    llvm::SmallString<128> ObjPath;
    if (auto EC = llvm::sys::fs::createTemporaryFile("textstats_clang", "o",
                                                     ObjFD, ObjPath)) {
        llvm::errs() << "创建临时文件失败: " << EC.message() << "\n";
        return 1;
    }
    {
        llvm::raw_fd_ostream ObjFile(ObjFD, /*shouldClose=*/true);
        ObjFile.write(ObjData.data(), ObjData.size());
    }
    llvm::outs() << "[2/3] LLVM 后端出目标文件: " << ObjPath << "\n";

    // ------------------------------------------------------------------
    //  Step 4: LLD-as-a-library —— 链接成 libtextstats.so
    // ------------------------------------------------------------------
    // 入参约定与命令行 ld.lld 完全一致（首个元素相当于 argv[0]）。
    //
    // 四种输入的链接语义一览：
    //   ObjPath            clang 刚编出的 .o  —— 并入产物
    //   CONSONANTS_OBJ_PATH 预先存在的 .o     —— 并入产物
    //   DIGITS_A_PATH      静态库 .a          —— 按需提取成员并入产物
    //   SPACES_SO_PATH     共享库 .so         —— 记为 DT_NEEDED 依赖
    const char *LldArgs[] = {
        "ld.lld",
        "-shared",
        "--rpath=$ORIGIN",        // 运行时在同目录找 libspaces.so
        "-o", OUTPUT_SO_PATH,
        ObjPath.c_str(),
        CONSONANTS_OBJ_PATH,
        DIGITS_A_PATH,
        SPACES_SO_PATH,
    };
    bool LinkOk = lld::elf::link(LldArgs, llvm::outs(), llvm::errs(),
                                 /*exitEarly=*/false, /*disableOutput=*/false);

    // 链接完成，临时目标文件没用了
    llvm::sys::fs::remove(ObjPath);

    if (!LinkOk) {
        llvm::errs() << "LLD 链接失败\n";
        return 1;
    }
    llvm::outs() << "[3/3] LLD 链接完成: " << OUTPUT_SO_PATH << "\n";

    return 0;
}
