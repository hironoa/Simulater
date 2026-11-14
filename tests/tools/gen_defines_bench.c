/* gen_defines_bench.c - defines（ヘッダの直アドレス定義とコンパイルスイッチ）の速度を測るための合成データを作る。
 *
 *   gen_defines_bench <出力フォルダ> [ファイル数 = 2000] [ヘッダ 1 個あたりのマクロ数 = 120]
 *
 * 出力フォルダの下に、40 個のサブフォルダに分けてヘッダ（全体の 6 割）と .c（残り）を作る。
 *   - ヘッダ: インクルードガード、bench_config.h と前のヘッダ 2 個の #include、typedef、
 *     #if PRODUCT == 1 / #elif / #else で番地の変わる直アドレスのマクロ、Shift_JIS のコメント。
 *   - bench_all.h: すべてのヘッダを #include する。
 *   - .c: 偶数番は bench_all.h を、奇数番は 30 個のヘッダを #include する（ほかは関数の本体らしい行）。
 * 測り方（例）:
 *   gen_defines_bench /tmp/bench 2000
 *   printf 'defines = /tmp/bench\ncdef = PRODUCT=2\n' > /tmp/bench.cfg
 *   sim info --abs X.lmf --cfg /tmp/bench.cfg        （"defines time:" の行に内訳が出る）
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0777)
#endif

#define DIRS 40
/* Shift_JIS の「機能表ソ」（2 バイト目が 0x5C の文字を含む） */
static const char sjis[] = "\x8B\x40\x94\x5C\x95\x5C\x83\x5C";

static FILE *open_out(const char *root, int dir, const char *name)
{
    char path[1024];
    FILE *f;
    if (dir < 0) snprintf(path, sizeof path, "%s/%s", root, name);
    else snprintf(path, sizeof path, "%s/m%02d/%s", root, dir, name);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    return f;
}

int main(int argc, char **argv)
{
    int files = argc > 2 ? atoi(argv[2]) : 2000, macros = argc > 3 ? atoi(argv[3]) : 120;
    int headers, sources, k, j;
    char path[1024], name[64];
    const char *root;
    FILE *f, *all;
    if (argc < 2 || files < 10 || macros < 1) {
        fprintf(stderr, "usage: gen_defines_bench <outdir> [files=2000] [macros-per-header=120]\n");
        return 2;
    }
    root = argv[1];
    make_dir(root);
    for (k = 0; k < DIRS; k++) { snprintf(path, sizeof path, "%s/m%02d", root, k); make_dir(path); }
    headers = files * 6 / 10; sources = files - headers - 2;

    f = open_out(root, -1, "bench_config.h");
    fprintf(f, "/* %s: build switches */\n#ifndef BENCH_CONFIG_H\n#define BENCH_CONFIG_H\n", sjis);
    fprintf(f, "#if PRODUCT == 2\n#define BENCH_WIDE 1\n#endif\n#define BENCH_BASE 0xF0000UL\n#define BENCH_OPT_1 1\n#define BENCH_OPT_3 1\n#endif\n");
    fclose(f);

    all = open_out(root, -1, "bench_all.h");
    fprintf(all, "#ifndef BENCH_ALL_H\n#define BENCH_ALL_H\n");
    for (k = 0; k < headers; k++) {
        snprintf(name, sizeof name, "bench_h%d.h", k);
        fprintf(all, "#include \"%s\"\n", name);
        f = open_out(root, k % DIRS, name);
        fprintf(f, "/* %s: %s %d */\n#ifndef BENCH_H%d_H\n#define BENCH_H%d_H\n#include \"bench_config.h\"\n", name, sjis, k, k, k);
        if (k > 1) fprintf(f, "#include \"bench_h%d.h\"\n#include \"m%02d/bench_h%d.h\"\n", k / 2, (k / 3) % DIRS, k / 3);
        fprintf(f, "#ifdef BENCH_WIDE\ntypedef unsigned short bench_w%d_t;\n#else\ntypedef unsigned char bench_w%d_t;\n#endif\n", k, k);
        fprintf(f, "typedef struct {\n    unsigned char b0:1; unsigned char b1:1; unsigned char b2:1; unsigned char b3:1;\n"
                   "    unsigned char b4:1; unsigned char b5:1; unsigned char b6:1; unsigned char b7:1;\n} bench_bits%d_t;\n", k);
        for (j = 0; j < macros; j++) {
            unsigned addr = 0xF0000u + (unsigned)((k * 131 + j * 8) % 0xFD00);
            fprintf(f, "/* %s %d-%d: register description, units and range (%s) */\n", sjis, k, j, sjis);
            if (j % 3 == 0) {
                fprintf(f, "#if PRODUCT == 1\n#define BENCH_%d_%d (*(volatile bench_w%d_t *)0x%05X)\n", k, j, k, addr);
                fprintf(f, "#elif PRODUCT == 2\n#define BENCH_%d_%d (*(volatile bench_w%d_t *)0x%05X)\n", k, j, k, addr + 2);
                fprintf(f, "#else\n#define BENCH_%d_%d (*(volatile bench_w%d_t *)0x%05X)\n#endif\n", k, j, k, addr + 4);
            } else if (j % 3 == 1) {
                fprintf(f, "#if defined(BENCH_OPT_%d) && !defined(BENCH_NO_%d)\n", j % 7, j % 5);
                fprintf(f, "#define BENCH_%d_%d (((volatile bench_bits%d_t *)0x%05X)->b%d)   // %s\n", k, j, k, addr, j % 8, sjis);
                fprintf(f, "#else\n#define BENCH_%d_%d (((volatile bench_bits%d_t *)0x%05X)->b%d)\n#endif\n", k, j, k, addr + 1, j % 8);
            } else {
                fprintf(f, "#define BENCH_%d_%d (*(volatile unsigned char *)(BENCH_BASE + 0x%04X))\n", k, j, addr - 0xF0000u);
                fprintf(f, "#define BENCH_%d_%d_MASK 0x%02XU\n", k, j, (unsigned)(j * 37) & 0xFFu);
            }
        }
        fprintf(f, "#endif /* BENCH_H%d_H */\n", k);
        fclose(f);
    }
    fprintf(all, "#endif\n");
    fclose(all);

    for (k = 0; k < sources; k++) {
        snprintf(name, sizeof name, "bench_c%d.c", k);
        f = open_out(root, k % DIRS, name);
        fprintf(f, "/* %s: %s %d */\n", name, sjis, k);
        if (k % 2 == 0) fprintf(f, "#include \"bench_all.h\"\n");
        else for (j = 0; j < 30; j++) fprintf(f, "#include \"bench_h%d.h\"\n", (k * 7 + j * 13) % headers);
        fprintf(f, "#define BENCH_LOCAL_%d 0x%04X\n#ifdef BENCH_DEBUG\n#define BENCH_TRACE_%d 1\n#endif\n", k, k & 0xFFFF, k);
        for (j = 0; j < 40; j++) {
            fprintf(f, "/* %s step %d */\nstatic unsigned char bench_f%d_%d(unsigned char x)\n{\n", sjis, j, k, j);
            fprintf(f, "    unsigned char y = (unsigned char)(x + %d);\n#if PRODUCT == 1\n    y ^= 0x%02XU;\n#endif\n", j, (unsigned)(j * 5) & 0xFFu);
            fprintf(f, "    if (y > 0x%02XU) { y = (unsigned char)(y >> 1); } /* %s */\n    return y;\n}\n", (unsigned)(j * 3) & 0xFFu, sjis);
        }
        fclose(f);
    }
    printf("gen_defines_bench: %d headers, %d sources, 2 common headers under %s\n", headers, sources, root);
    return 0;
}
