/* check_native_layout.c - ロードモジュールの変数と DLL の変数の対応づけ（native_layout_match）の境界ケース */
#include "native_layout.h"

#include <stdio.h>
#include <string.h>

static int cases, failed;

#define N(a) (sizeof (a) / sizeof (a)[0])

static void expect(const char *what, const native_layout_var *lm, size_t nlm, const native_layout_var *dll, size_t ndll,
                   const long *want)
{
    long got[16];
    size_t i;
    int bad = 0;
    cases++;
    native_layout_match(lm, nlm, dll, ndll, got);
    for (i = 0; i < nlm; i++) if (got[i] != want[i]) bad = 1;
    if (!bad) return;
    failed++;
    printf("NG %s:", what);
    for (i = 0; i < nlm; i++) printf(" %s=%ld（期待 %ld）", lm[i].name, got[i], want[i]);
    printf("\n");
}

int main(void)
{
    /* 製品の DLL の変数（product = 1）と、native_src の変数（product = 0） */
    static const native_layout_var dll[] = {
        {"g_count", "C:/prj/src/app.c", NULL, 1, 1},                 /* 0 グローバル */
        {"s_state", "C:/prj/src/app.c", NULL, 0, 1},                 /* 1 app.c のファイル内 static */
        {"s_state", "C:/prj/src/io.c", NULL, 0, 1},                  /* 2 io.c の同じ名前のファイル内 static */
        {"cnt", "C:/prj/src/app.c", "f_static1", 0, 1},              /* 3 関数の中の static */
        {"cnt", "C:/prj/src/app.c", "f_static2", 0, 1},              /* 4 同じ名前で別の関数 */
        {"cnt", "C:/prj/src/io.c", "f_static2", 0, 1},               /* 5 同じ名前、同じ関数名で別のファイル */
        {"drv_value", "C:/prj/test/drv.c", NULL, 1, 0},              /* 6 native_src のグローバル */
        {"s_drv", "C:/prj/test/drv.c", NULL, 0, 0},                  /* 7 native_src のファイル内 static */
        {"cnt", "C:/prj/test/drv.c", "f_static2", 0, 0},             /* 8 native_src の関数の中の static */
        {"only_dll", "C:/prj/src/app.c", NULL, 1, 1},                /* 9 ロードモジュールにない */
        {"g_mixed", "C:/prj/test/drv.c", NULL, 1, 0},                /* 10 native_src のグローバル */
        {"g_mixed", "C:/prj/src/app.c", NULL, 1, 1},                 /* 11 製品のグローバル（同じ名前） */
    };
    {   /* 名前、ファイル名、関数名の組で選ぶ。ロードモジュールの並びに左右されない */
        static const native_layout_var lm[] = {
            {"cnt", "io.c", "f_static2", 0, 0},
            {"s_state", "IO.C", NULL, 0, 0},
            {"cnt", "app.c", "f_static2", 0, 0},
            {"cnt", "app.c", "f_static1", 0, 0},
            {"s_state", "src\\app.c", NULL, 0, 0},
            {"g_count", "app.c", NULL, 1, 0},
        };
        static const long want[] = {5, 2, 4, 3, 1, 0};
        expect("static は関数名とファイル名を合わせる", lm, N(lm), dll, N(dll), want);
    }
    {   /* native_src のソースの変数は、ロードモジュールに同じ名前があっても対象外 */
        static const native_layout_var lm[] = {
            {"drv_value", "drv.c", NULL, 1, 0},
            {"s_drv", "drv.c", NULL, 0, 0},
            {"cnt", "drv.c", "f_static2", 0, 0},
            {"g_mixed", "app.c", NULL, 1, 0},
            {"no_such", "app.c", NULL, 1, 0},
        };
        static const long want[] = {NATIVE_LAYOUT_SRC, NATIVE_LAYOUT_SRC, NATIVE_LAYOUT_SRC, 11, NATIVE_LAYOUT_NONE};
        expect("native_src の変数は対象外", lm, N(lm), dll, N(dll), want);
    }
    {   /* 違う関数名、違うファイル、関数の中の static とファイル内 static の取り違えをしない */
        static const native_layout_var lm[] = {
            {"cnt", "app.c", "f_static3", 0, 0},
            {"cnt", "app.c", NULL, 0, 0},
            {"s_state", "main.c", NULL, 0, 0},
            {"s_state", "app.c", "f_static1", 0, 0},
        };
        static const long want[] = {NATIVE_LAYOUT_NONE, NATIVE_LAYOUT_NONE, NATIVE_LAYOUT_NONE, NATIVE_LAYOUT_NONE};
        expect("違う関数名やファイルには当てない", lm, N(lm), dll, N(dll), want);
    }
    {   /* 1 つの DLL の変数は 1 度だけ使う。2 つ目のロードモジュールの変数は対応なし */
        static const native_layout_var lm[] = {
            {"g_count", "app.c", NULL, 1, 0},
            {"g_count", "app.c", NULL, 1, 0},
        };
        static const long want[] = {0, NATIVE_LAYOUT_NONE};
        expect("同じ DLL の変数は 1 度だけ", lm, N(lm), dll, N(dll), want);
    }
    {   /* ファイル名がない static は当てない。グローバルはファイル名がなくても当てる */
        static const native_layout_var lm[] = {
            {"s_state", "", NULL, 0, 0},
            {"g_count", NULL, NULL, 1, 0},
        };
        static const long want[] = {NATIVE_LAYOUT_NONE, 0};
        expect("ファイル名のない変数", lm, N(lm), dll, N(dll), want);
    }
    {   /* ファイル名の部分だけが同じ（パスが違う）なら、末尾の一致がないときに当てる */
        static const native_layout_var lm[] = {
            {"s_state", "D:/build/obj/../src/app.c", NULL, 0, 0},
        };
        static const long want[] = {1};
        expect("ファイル名の部分の一致", lm, N(lm), dll, N(dll), want);
    }
    {   /* DLL の変数がない */
        static const native_layout_var lm[] = {{"g_count", "app.c", NULL, 1, 0}};
        static const long want[] = {NATIVE_LAYOUT_NONE};
        expect("DLL の変数がない", lm, N(lm), dll, 0, want);
    }
    printf("native layout: %d cases, %d failed\n", cases, failed);
    return failed ? 1 : 0;
}
