/*
 * test_loader.c —— 验证程序
 *
 * libtextstats.so 是运行 make_libtextstats 才生成的，构建期并不存在，
 * 所以这里用 dlopen/dlsym 动态加载、取符号，而不是构建期链接。
 *
 * 同时它也顺带验证了传递依赖：libtextstats.so 依赖 libspaces.so
 * （DT_NEEDED + RUNPATH=$ORIGIN），dlopen 成功即说明传递解析正常。
 */

#include <dlfcn.h>
#include <stdio.h>

int main(void) {
    void *h = dlopen(TEXTSTATS_SO_PATH, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen 失败: %s\n", dlerror());
        return 1;
    }

    typedef void (*AnalyzeFn)(const char *, int *);
    AnalyzeFn analyze = (AnalyzeFn)dlsym(h, "ts_analyze");
    if (!analyze) {
        fprintf(stderr, "dlsym 失败: %s\n", dlerror());
        return 1;
    }

    const char *text = "hello world 42";
    int st[4];
    analyze(text, st);

    printf("文本: \"%s\"\n", text);
    printf("元音=%d 辅音=%d 数字=%d 空格=%d\n", st[0], st[1], st[2], st[3]);
    /* 预期: 元音=3(e,o,o) 辅音=7(h,l,l,w,r,l,d) 数字=2(4,2) 空格=2 */
    return 0;
}
