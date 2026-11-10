/*
 * payload.c —— llvmcall 形式三（bitcode）的载荷
 *
 * 由 run.sh 用 clang 编成 LLVM bitcode（build/payload.bc），
 * Julia 侧读字节后通过 Base.llvmcall((bytes, "triple"), ...) 调用。
 *
 * 用 C（而非 C++）编译：函数名不经名字修饰，入口名就是 "triple"。
 */

int triple(int x) { return x * 3; }
