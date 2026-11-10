/*
 * textstats.c —— 文本统计库的"主"源文件（.c 角色）
 *
 * 本文件【不】经过构建期编译：它由 make_libtextstats 在运行期用
 * clang-as-a-library（libclang-cpp）编译成 LLVM IR，再经 LLVM 后端
 * 生成目标文件，最后由 LLD 与其余产物链接成 libtextstats.so。
 */

/* 来自其它产物的函数，链接成共享库时解析：
 *   ts_count_consonants ← consonants.o （预编译目标文件）
 *   ts_count_digits     ← libdigits.a  （静态库，按需提取成员）
 *   ts_count_spaces     ← libspaces.so （共享库，成为运行时依赖）
 */
extern int ts_count_consonants(const char *s);
extern int ts_count_digits(const char *s);
extern int ts_count_spaces(const char *s);

/* 本文件自己实现的导出函数：统计元音字母个数 */
int ts_count_vowels(const char *s) {
    int n = 0;
    for (; *s; ++s) {
        char c = *s;
        if (c=='a'||c=='e'||c=='i'||c=='o'||c=='u'||
            c=='A'||c=='E'||c=='I'||c=='O'||c=='U')
            ++n;
    }
    return n;
}

/* 聚合入口：一次性统计四类字符
 * out[0]=元音 out[1]=辅音 out[2]=数字 out[3]=空格 */
void ts_analyze(const char *s, int out[4]) {
    out[0] = ts_count_vowels(s);
    out[1] = ts_count_consonants(s);
    out[2] = ts_count_digits(s);
    out[3] = ts_count_spaces(s);
}
