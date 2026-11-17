/* check_native_layout.c - ロードモジュールの変数と DLL の変数の対応づけ（native_layout_match）の境界ケース */
#include "native_layout.h"

#include <stdio.h>
#include <string.h>

static int cases, failed;

#define N(a) (sizeof (a) / sizeof (a)[0])
#define NONE NATIVE_LAYOUT_NONE
#define SRC NATIVE_LAYOUT_SRC
#define AMB NATIVE_LAYOUT_AMBIGUOUS

/* lm をそのままの順と逆の順の 2 通りで対応づけ、どちらも want になること（結果が lm の並びに左右されないこと） */
static void expect(const char *what, const native_layout_var *lm, size_t nlm, const native_layout_var *dll, size_t ndll,
                   const long *want)
{
    native_layout_var rev[16];
    long got[16], got_rev[16];
    size_t i;
    int bad = 0;
    cases++;
    for (i = 0; i < nlm; i++) rev[i] = lm[nlm - 1 - i];
    native_layout_match(lm, nlm, dll, ndll, got);
    native_layout_match(rev, nlm, dll, ndll, got_rev);
    for (i = 0; i < nlm; i++) if (got[i] != want[i] || got_rev[nlm - 1 - i] != want[i]) bad = 1;
    if (!bad) return;
    failed++;
    printf("NG %s:", what);
    for (i = 0; i < nlm; i++)
        printf(" %s[%s]=%ld/%ld（期待 %ld）", lm[i].name, lm[i].file ? lm[i].file : "", got[i], got_rev[nlm - 1 - i], want[i]);
    printf("\n");
}

static void expect_tail(const char *a, const char *b, int want)
{
    int got = native_layout_common_tail(a, b);
    cases++;
    if (got == want) return;
    failed++;
    printf("NG common_tail(%s, %s) = %d（期待 %d）\n", a ? a : "NULL", b ? b : "NULL", got, want);
}

int main(void)
{
    expect_tail("C:/prj/src/app.c", "src\\app.c", 2);
    expect_tail("C:/prj/src/app.c", "app.c", 1);
    expect_tail("C:/prj/src/app.c", "C:/prj/test/app.c", 1);
    expect_tail("C:/prj/src/app.c", "C:/prj/src/io.c", 0);
    expect_tail("C:/prj/src/app.c", "", 0);
    expect_tail(NULL, "app.c", 0);
    expect_tail("C:/prj/src/app.c", "C:\\prj\\src\\app.c", 4);
    expect_tail("C:/prj/src//app.c", "src/app.c", 2);
    expect_tail("x/myapp.c", "app.c", 0);
#ifdef _WIN32
    expect_tail("C:/PRJ/SRC/APP.C", "src\\app.c", 2);
#else
    expect_tail("C:/PRJ/SRC/APP.C", "src/app.c", 0);
#endif

    {   /* レビューの例: 同じファイル名・同じ名前の static が、製品（src/app.c）と native_src（test/app.c）の両方にある */
        static const native_layout_var dll[] = {
            {"state", "C:/prj/src/app.c", NULL, 0, 1},      /* 0 製品 */
            {"state", "C:/prj/test/app.c", NULL, 0, 0},     /* 1 native_src */
        };
        static const native_layout_var lm_path[] = {
            {"state", "C:/prj/test/app.c", NULL, 0, 0},
            {"state", "C:/prj/src/app.c", NULL, 0, 0},
        };
        static const long want_path[] = {SRC, 0};
        static const native_layout_var lm_rel[] = {
            {"state", "src\\app.c", NULL, 0, 0},
            {"state", "test/app.c", NULL, 0, 0},
        };
        static const long want_rel[] = {0, SRC};
        static const native_layout_var lm_one[] = {{"state", "C:/prj/test/app.c", NULL, 0, 0}};
        static const long want_one[] = {SRC};
        /* ロードモジュールが名前だけ（CA78K0R の .lmf）なら区別できない */
        static const native_layout_var lm_name[] = {
            {"state", "app.c", NULL, 0, 0},
            {"state", "app.c", NULL, 0, 0},
        };
        static const long want_name[] = {AMB, AMB};
        static const native_layout_var lm_name1[] = {{"state", "app.c", NULL, 0, 0}};
        static const long want_name1[] = {AMB};
        expect("製品とドライバ: パスつき", lm_path, N(lm_path), dll, N(dll), want_path);
        expect("製品とドライバ: 相対パス", lm_rel, N(lm_rel), dll, N(dll), want_rel);
        expect("製品とドライバ: ドライバの側だけ", lm_one, N(lm_one), dll, N(dll), want_one);
        expect("製品とドライバ: 名前だけ", lm_name, N(lm_name), dll, N(dll), want_name);
        expect("製品とドライバ: 名前だけ 1 つ", lm_name1, N(lm_name1), dll, N(dll), want_name1);
    }
    {   /* 製品の側にだけ同じファイル名が 2 つある */
        static const native_layout_var dll[] = {
            {"state", "C:/prj/src/a/app.c", NULL, 0, 1},    /* 0 */
            {"state", "C:/prj/src/b/app.c", NULL, 0, 1},    /* 1 */
        };
        static const native_layout_var lm_path[] = {
            {"state", "D:/build/src/b/app.c", NULL, 0, 0},
            {"state", "a/app.c", NULL, 0, 0},
        };
        static const long want_path[] = {1, 0};
        static const native_layout_var lm_name[] = {{"state", "app.c", NULL, 0, 0}};
        static const long want_name[] = {AMB};
        expect("製品の同じファイル名: パスつき", lm_path, N(lm_path), dll, N(dll), want_path);
        expect("製品の同じファイル名: 名前だけ", lm_name, N(lm_name), dll, N(dll), want_name);
    }
    {   /* 関数の中の static: 関数名だけ違う組、ファイル名だけ違う組 */
        static const native_layout_var dll[] = {
            {"cnt", "C:/prj/src/app.c", "f_static1", 0, 1},     /* 0 */
            {"cnt", "C:/prj/src/app.c", "f_static2", 0, 1},     /* 1 関数名だけ違う */
            {"cnt", "C:/prj/src/io.c", "f_static2", 0, 1},      /* 2 ファイル名だけ違う */
            {"cnt", "C:/prj/test/drv.c", "f_static2", 0, 0},    /* 3 native_src */
            {"cnt", "C:/prj/src/app.c", NULL, 0, 1},            /* 4 ファイル内 static */
        };
        static const native_layout_var lm[] = {
            {"cnt", "io.c", "f_static2", 0, 0},
            {"cnt", "app.c", "f_static2", 0, 0},
            {"cnt", "app.c", "f_static1", 0, 0},
            {"cnt", "drv.c", "f_static2", 0, 0},
            {"cnt", "app.c", "f_static3", 0, 0},
            {"cnt", "main.c", "f_static2", 0, 0},
            {"cnt", "app.c", NULL, 0, 0},
        };
        static const long want[] = {2, 1, 0, SRC, NONE, NONE, 4};
        expect("関数の中の static", lm, N(lm), dll, N(dll), want);
    }
    {   /* グローバル: ファイル名を見ない。native_src で定義されていれば対象外 */
        static const native_layout_var dll[] = {
            {"g_count", "C:/prj/src/app.c", NULL, 1, 1},        /* 0 製品 */
            {"drv_value", "C:/prj/test/drv.c", NULL, 1, 0},     /* 1 native_src */
            {"s_only", "C:/prj/src/app.c", NULL, 0, 1},         /* 2 製品の static（ロードモジュールではグローバル） */
        };
        static const native_layout_var lm[] = {
            {"g_count", "other.c", NULL, 1, 0},
            {"drv_value", "", NULL, 1, 0},
            {"s_only", "app.c", NULL, 1, 0},
            {"no_such", "app.c", NULL, 1, 0},
        };
        static const long want[] = {0, SRC, 2, NONE};
        expect("グローバル", lm, N(lm), dll, N(dll), want);
    }
    {   /* ロードモジュールに同じグローバルが 2 つ載っていれば、どちらとも決めない */
        static const native_layout_var dll[] = {{"g_count", "C:/prj/src/app.c", NULL, 1, 1}};
        static const native_layout_var lm[] = {
            {"g_count", "app.c", NULL, 1, 0},
            {"g_count", "app.c", NULL, 1, 0},
        };
        static const long want[] = {AMB, AMB};
        expect("同じ DLL の変数を 2 つが求める", lm, N(lm), dll, N(dll), want);
    }
    {   /* 強い組が先に決まり、使った DLL の変数は弱い組に使わない（並びに左右されない） */
        static const native_layout_var dll[] = {
            {"state", "C:/prj/src/app.c", NULL, 0, 1},      /* 0 */
            {"state", "C:/prj/lib/app.c", NULL, 0, 1},      /* 1 */
        };
        static const native_layout_var lm[] = {
            {"state", "x/app.c", NULL, 0, 0},               /* 強さ 1 で 0 と 1 の両方 → あいまい */
            {"state", "prj/src/app.c", NULL, 0, 0},         /* 強さ 3 で 0 */
        };
        static const long want[] = {1, 0};
        expect("強い組を先に確定", lm, N(lm), dll, N(dll), want);
    }
    {   /* 大文字小文字と区切りの違い */
        static const native_layout_var dll[] = {{"s_state", "C:/prj/src/app.c", NULL, 0, 1}};
        static const native_layout_var lm[] = {{"s_state", "SRC\\APP.C", NULL, 0, 0}};
#ifdef _WIN32
        static const long want[] = {0};
#else
        static const long want[] = {NONE};
#endif
        expect("大文字小文字と区切り", lm, N(lm), dll, N(dll), want);
    }
    {   /* ファイル名のない static は当てない。DLL の変数がない */
        static const native_layout_var dll[] = {{"s_state", "C:/prj/src/app.c", NULL, 0, 1}};
        static const native_layout_var lm[] = {{"s_state", "", NULL, 0, 0}};
        static const long want[] = {NONE};
        expect("ファイル名のない static", lm, N(lm), dll, N(dll), want);
        expect("DLL の変数がない", lm, N(lm), dll, 0, want);
    }
    printf("native layout: %d cases, %d failed\n", cases, failed);
    return failed ? 1 : 0;
}
