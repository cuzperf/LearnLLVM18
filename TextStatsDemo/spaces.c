/*
 * spaces.c —— 构建期由 C 编译器编成 libspaces.so（.so 共享库角色）
 *
 * 统计空格个数。链接 libtextstats.so 时，本库不会被并入产物，
 * 而是被记录为 DT_NEEDED 运行时依赖（运行 libtextstats.so 时
 * 动态链接器会去加载 libspaces.so）。
 */

int ts_count_spaces(const char *s) {
    int n = 0;
    for (; *s; ++s) {
        if (*s == ' ')
            ++n;
    }
    return n;
}
