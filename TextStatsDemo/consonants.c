/*
 * consonants.c —— 构建期由 C 编译器编成 consonants.o（.o 角色）
 *
 * 故意用 gcc 而非 clang 构建：ELF 目标文件是标准格式，
 * 与编译器品牌无关，LLD 照常链接。
 *
 * 统计英文字母中的辅音个数。
 */

int ts_count_consonants(const char *s) {
    int n = 0;
    for (; *s; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';                 /* 转小写统一判断 */
        if (c >= 'a' && c <= 'z' &&
            c!='a' && c!='e' && c!='i' && c!='o' && c!='u')
            ++n;
    }
    return n;
}
