# TextStatsDemo 说明文档

把四种来源的代码产物（`.c` 源码、`.o` 目标文件、`.a` 静态库、`.so` 共享库）
汇聚成一个共享库 `libtextstats.so`。全程在进程内完成，不 fork 任何外部工具。

整体结构与运转流程见同目录的 `architecture.excalidraw`。

## 文件清单

| 文件 | 作用 |
|---|---|
| `textstats.c` | 主源文件（聚合入口 `ts_analyze`），**构建期不编译**，运行期由 clang-as-a-library 编译 |
| `consonants.c` | 构建期由 gcc 编成 `consonants.o`（.o 角色） |
| `digits.c` | 构建期由 gcc 编成 `libdigits.a`（.a 角色） |
| `spaces.c` | 构建期由 gcc 编成 `libspaces.so`（.so 角色） |
| `make_libtextstats.cpp` | 生成工具：clang 库编译 .c → LLVM 后端出 .o → LLD 链接 |
| `test_loader.c` | 验证程序：dlopen 产物并调用导出函数 |
| `build.sh` | 一键构建 + 生成 + 验证（`clean`/`debug`/`release`） |
| `architecture.excalidraw` | 架构与流程图 |

## 本示例用到的 LLVM 能力

### 1. clang-as-a-library（libclang-cpp）

`make_libtextstats.cpp` Step 2：在同一进程内完成 "C 源码 → AST → LLVM IR"。

- 关键类：`clang::CompilerInstance` + `clang::CompilerInvocation` +
  `clang::EmitLLVMOnlyAction`（只产出 LLVM IR，不出机器码）。
- 交互式 C++ 解释器 `clang-repl` 用的就是这套机制（另加增量编译）。
- 需要 `libclang-19-dev`，链接 `clang-cpp`（ClangConfig.cmake 导出的组合库目标）。

### 2. LLVM 后端代码生成（TargetMachine）

`make_libtextstats.cpp` Step 3：把 `llvm::Module` 编成可重定位目标文件。

- 关键接口：`llvm::TargetRegistry::lookupTarget` →
  `TargetMachine::addPassesToEmitFile`（输出到内存 buffer）。
- 同一个后端既能服务 JIT（主项目 `jit_*_demo`），也能服务 AOT（本示例）。

### 3. LLD-as-a-library（lldELF）

`make_libtextstats.cpp` Step 4：在进程内完成链接。

- 关键接口：`lld::elf::link`（`lld/Common/Driver.h`），参数与命令行
  `ld.lld` 完全一致；`LLD_HAS_DRIVER(elf)` 宏提供原型声明。
- 需要 `liblld-19-dev`，链接 `lldELF` + `lldCommon`（静态库目标）。
- 链接语义与命令行一致：`.o` 并入产物、`.a` 按需提取成员、`.so` 记为
  `DT_NEEDED` 运行时依赖。

### 4. （延伸）ORC JIT

如果目的是"生成代码立即执行"而不是落盘 `.so`，应该用 ORC JIT
（`LLJIT`、`StaticLibraryDefinitionGenerator`、`addObjectFile`、
`DynamicLibrarySearchGenerator` 等），见主项目根目录的 `jit_*_demo` 系列。

## 注意事项

### 兼容性

1. **LLVM / Clang / LLD 版本必须配套**（本项目统一用 19.1.x）。它们的
   C++ ABI 跨版本不兼容，混版本链接会出现莫名其妙的崩溃。CMake 里
   `LLVM_DIR` / `Clang_DIR` / `LLD_DIR` 三个包要指向同一套安装。
2. **LLVM IR 向后兼容、向前不兼容**：新版 libLLVM 能读旧版 clang 生成的
   `.ll`/`.bc`（自动升级），反过来不行。运行期编译源码时， clang 库与
   LLVM 库同版本，天然没有这个问题。
3. **`.o` / `.a` / `.so` 与编译器品牌无关**：它们是标准 ELF 格式，
   gcc 的产物 clang/LLD 照常处理。本示例有意用 gcc 构建三种二进制产物，
   就是为了证明这一点。
4. **`-flto` 产物是陷阱**：
   - gcc `-flto`（slim）：`.o` 的 `.text` 是空的，代码以 GIMPLE 字节码
     存在 `.gnu.lto_*` 节里 —— 链接器/对象层拿不到机器码；
   - clang `-flto`：产出的 `.o` 根本不是 ELF，是 LLVM bitcode；
   - gcc 可用 `-ffat-lto-objects`（机器码 + GIMPLE 并存）规避。
   - 结论：喂给链接器或 JIT 对象层的 `.o/.a`，不要用 `-flto` 编译。
5. **LLVM C++ API 跨版本会变动**（C API 相对稳定）。本项目实际遇到的两例：
   - `llvm/Support/Host.h` 在 LLVM 19 移到了 `llvm/TargetParser/Host.h`；
   - `MemoryBuffer::getFile` 返回 `ErrorOr`（带 `std::error_code`），
     不是 `Expected`，不能直接交给 `ExitOnErr`。
   升级 LLVM 版本时以编译错误为准逐项适配即可。

### 编译选项

1. **位置无关代码（PIC）是必须的**：产物要链接进共享库，所有输入都
   得是 PIC，否则链接时报 `R_X86_64_PC32 against ...` 之类的重定位错误。
   三处要同时保证：
   - clang 库：cc1 参数 `-pic-level 2`（等价于驱动参数 `-fPIC`）；
   - LLVM 后端：`createTargetMachine(..., llvm::Reloc::PIC_)`；
   - gcc 产物：CMake 里 `POSITION_INDEPENDENT_CODE ON`（即 `-fPIC`）。
2. **clang 库的参数是 cc1 风格，不是驱动风格**：
   `CompilerInvocation::CreateFromArgs` 按 cc1 可见性解析参数，
   **不含 argv[0] 程序名**。所以写 `-x c -std=c11 -triple ...`，
   而不是 `clang++ -x c ...`。
3. **头文件搜索路径要自备**：
   - `-resource-dir`（`clang++ -print-resource-dir` 可得）只覆盖 clang
     内置头（`stddef.h` 等）；
   - 源码若 `#include` 了 libc / C++ 标准库头，还需用 `-I` /
     `-internal-isystem` 补齐对应搜索路径（C++ 标准库头在 GCC 安装目录下）。
     本示例的 `textstats.c` 无任何 `#include`，所以最简配置即可。
4. **符号名 = 链接名**：C++ 有名字修饰（`gcd` → `_Z3gcdii`），
   `extern "C"` 的符号不做修饰。跨产物引用符号时，以
   `nm 产物文件` 看到的符号名为准。
5. **LLD 链接参数**：
   - `-shared` 产出共享库；
   - `--rpath=$ORIGIN` 让产物运行时优先在同目录找依赖（配合
     `DT_NEEDED libspaces.so`）；
   - 被依赖的 `.so` 要带 SONAME（CMake 的 SHARED 库默认会设置），
     否则 `DT_NEEDED` 会记成传给链接器的字面路径。

### 运行时

1. **传递依赖解析**：`test_loader` dlopen `libtextstats.so` 时，
   动态链接器按后者的 `RUNPATH=$ORIGIN` 找 `libspaces.so`。
   若部署目录结构不同，改用 `LD_LIBRARY_PATH` 或调整 rpath。
2. `dlopen` 用 `RTLD_NOW` 可以在加载时就暴露所有未定义符号，
   比首次调用时才报错更好排查。
3. LLD 的 `lld::elf::link(..., exitEarly=false, ...)` 单次调用没问题；
   若要在一个进程里链接多次，建议改用支持重入的 `lld::lldMain`
   （见 `lld/Common/Driver.h` 的 `Result::canRunAgain` 说明）。

## 构建与验证

```bash
./build.sh          # Debug 构建 + 生成 libtextstats.so + 三步验证
./build.sh release  # Release
./build.sh clean    # 清理
```

手动等价命令：

```bash
cmake -S . -B build && cmake --build build -j
./build/make_libtextstats                 # 生成 libtextstats.so
readelf -d build/libtextstats.so          # 看 NEEDED / RUNPATH
nm -D build/libtextstats.so | grep ts_    # 看符号去向（T=并入，U=留给 .so）
./build/test_loader                       # dlopen 运行验证
```

依赖：`cmake >= 3.20`、`gcc`、`clang-19`、`libclang-19-dev`、
`liblld-19-dev`，以及 LLVM 19 开发包（提供 LLVMConfig.cmake）。
三个包路径均可在 cmake 配置时用 `-DLLVM_DIR=... -DClang_DIR=...
-DLLD_DIR=...` 覆盖。
