/*
 * digits.c —— 构建期由 C 编译器编成 libdigits.a（.a 静态库角色）
 *
 * 统计数字字符个数。链接共享库时，归档中只有被引用的成员
 * 会被提取并并入产物（与静态链接器语义一致）。
 */

int ts_count_digits(const char *s) {
    int n = 0;
    for (; *s; ++s) {
        if (*s >= '0' && *s <= '9')
            ++n;
    }
    return n;
}
