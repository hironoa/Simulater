/* check_native_near.c - LLVM IR の inttoptr の読み替え（native_near）の境界ケース
 * 入力の多くは clang 18.1.8 が出した IR の行そのもの（-O0、または -O2 -Xclang -disable-llvm-passes）。
 * clang の C のフロントエンドは inttoptr を i64 でしか出さないので、i32 / i16 の行は、その行の型だけを変えたもの。 */
#include "native_near.h"
#include <stdio.h>
#include <string.h>

#define DECL "\ndeclare i64 @__sim_near(i64) nounwind willreturn memory(none)\n"

static const struct { const char *in, *out; int cnst, dyn, kept; } cases[] = {
    /* 定数の形: 命令のオペランド（tests/native の near.c、io.c） */
    {"  %1 = load volatile i16, ptr inttoptr (i64 65492 to ptr), align 2, !dbg !54\n",
     "  %1 = load volatile i16, ptr inttoptr (i64 1048532 to ptr), align 2, !dbg !54\n", 1, 0, 0},
    {"  store volatile i16 %3, ptr inttoptr (i64 65492 to ptr), align 2, !dbg !63\n",
     "  store volatile i16 %3, ptr inttoptr (i64 1048532 to ptr), align 2, !dbg !63\n", 1, 0, 0},
    {"  %1 = load volatile i8, ptr inttoptr (i64 256 to ptr), align 1, !dbg !62\n",
     "  %1 = load volatile i8, ptr inttoptr (i64 983296 to ptr), align 1, !dbg !62\n", 1, 0, 0},
    /* 5 桁の番地（0x10000 以上）は変えない */
    {"  store volatile i8 %3, ptr inttoptr (i64 1048321 to ptr), align 1, !dbg !39\n",
     "  store volatile i8 %3, ptr inttoptr (i64 1048321 to ptr), align 1, !dbg !39\n", 0, 0, 0},
    /* getelementptr の定数式の中 */
    {"  %1 = load volatile i16, ptr getelementptr inbounds (%struct.anon.0, ptr inttoptr (i64 768 to ptr), i32 0, i32 1), align 2, !dbg !66\n",
     "  %1 = load volatile i16, ptr getelementptr inbounds (%struct.anon.0, ptr inttoptr (i64 983808 to ptr), i32 0, i32 1), align 2, !dbg !66\n", 1, 0, 0},
    /* グローバルの初期値（1 行に複数。65535 は書き換え、65536 はそのまま） */
    {"@gtab = dso_local global [3 x ptr] [ptr inttoptr (i64 1 to ptr), ptr inttoptr (i64 65535 to ptr), ptr inttoptr (i64 65536 to ptr)], align 16, !dbg !22\n",
     "@gtab = dso_local global [3 x ptr] [ptr inttoptr (i64 983041 to ptr), ptr inttoptr (i64 1048575 to ptr), ptr inttoptr (i64 65536 to ptr)], align 16, !dbg !22\n", 2, 0, 0},
    {"@gp = dso_local global ptr inttoptr (i64 4660 to ptr), align 8, !dbg !0\n",
     "@gp = dso_local global ptr inttoptr (i64 987700 to ptr), align 8, !dbg !0\n", 1, 0, 0},
    {"@gbig = dso_local global ptr inttoptr (i64 983040 to ptr), align 8, !dbg !20\n",
     "@gbig = dso_local global ptr inttoptr (i64 983040 to ptr), align 8, !dbg !20\n", 0, 0, 0},
    /* 関数へのポインタ（呼び出し、ret） */
    {"  call void inttoptr (i64 4660 to ptr)() #2, !dbg !86\n",
     "  call void inttoptr (i64 987700 to ptr)() #2, !dbg !86\n", 1, 0, 0},
    {"  ret ptr inttoptr (i64 65535 to ptr), !dbg !91\n",
     "  ret ptr inttoptr (i64 1048575 to ptr), !dbg !91\n", 1, 0, 0},
    /* 0 と負の表記（i64 の -4 は 0xFFFFFFFFFFFFFFFC）は変えない */
    {"  ret ptr inttoptr (i64 0 to ptr), !dbg !9\n", "  ret ptr inttoptr (i64 0 to ptr), !dbg !9\n", 0, 0, 0},
    {"  %1 = load volatile i32, ptr inttoptr (i64 -4 to ptr), align 4, !dbg !53\n",
     "  %1 = load volatile i32, ptr inttoptr (i64 -4 to ptr), align 4, !dbg !53\n", 0, 0, 0},
    /* i16 / i32 の定数（LLVM は符号付きで書く。i16 の -44 は 0xFFD4）。型は i64 にする */
    {"  %1 = load volatile i16, ptr inttoptr (i16 -44 to ptr), align 2\n",
     "  %1 = load volatile i16, ptr inttoptr (i64 1048532 to ptr), align 2\n", 1, 0, 0},
    {"  %1 = load volatile i16, ptr inttoptr (i32 65492 to ptr), align 2\n",
     "  %1 = load volatile i16, ptr inttoptr (i64 1048532 to ptr), align 2\n", 1, 0, 0},
    {"  %1 = load volatile i16, ptr inttoptr (i32 -1 to ptr), align 2\n",
     "  %1 = load volatile i16, ptr inttoptr (i32 -1 to ptr), align 2\n", 0, 0, 0},
    /* 命令の形: i64（near.c の near_tbl_read） */
    {"  %8 = inttoptr i64 %7 to ptr, !dbg !81\n",
     "  %sim.near.0 = call i64 @__sim_near(i64 %7), !dbg !81\n  %8 = inttoptr i64 %sim.near.0 to ptr, !dbg !81\n" DECL, 0, 1, 0},
    /* i32 / i16 は i64 に広げてから。デバッグの印がなければ付けない */
    {"  %5 = inttoptr i32 %t32 to ptr\n",
     "  %sim.near.z0 = zext i32 %t32 to i64\n  %sim.near.0 = call i64 @__sim_near(i64 %sim.near.z0)\n  %5 = inttoptr i64 %sim.near.0 to ptr\n" DECL, 0, 1, 0},
    {"  %5 = inttoptr i16 %t16 to ptr, !dbg !62\n  %6 = inttoptr i64 %a.b to ptr, !dbg !63\n",
     "  %sim.near.z0 = zext i16 %t16 to i64, !dbg !62\n  %sim.near.0 = call i64 @__sim_near(i64 %sim.near.z0), !dbg !62\n"
     "  %5 = inttoptr i64 %sim.near.0 to ptr, !dbg !62\n"
     "  %sim.near.1 = call i64 @__sim_near(i64 %a.b), !dbg !63\n  %6 = inttoptr i64 %sim.near.1 to ptr, !dbg !63\n" DECL, 0, 2, 0},
    /* 命令の形で値が定数 */
    {"  %2 = inttoptr i64 512 to ptr, !dbg !5\n", "  %2 = inttoptr i64 983552 to ptr, !dbg !5\n", 1, 0, 0},
    {"  %2 = inttoptr i64 65536 to ptr, !dbg !5\n", "  %2 = inttoptr i64 65536 to ptr, !dbg !5\n", 0, 0, 0},
    /* 宣言がすでにあれば足さない。CRLF の行には CRLF で差し込む */
    {"declare i64 @__sim_near(i64)\r\n  %8 = inttoptr i64 %7 to ptr\r\n",
     "declare i64 @__sim_near(i64)\r\n  %sim.near.0 = call i64 @__sim_near(i64 %7)\r\n  %8 = inttoptr i64 %sim.near.0 to ptr\r\n", 0, 1, 0},
    /* 最後の行に改行がない（改行を足してから宣言を置く） */
    {"  %8 = inttoptr i64 %7 to ptr",
     "  %sim.near.0 = call i64 @__sim_near(i64 %7)\n  %8 = inttoptr i64 %sim.near.0 to ptr\n" DECL, 0, 1, 0},
    /* 文字列とコメントの中は見ない（clang が出した文字列の定数と comdat の名前） */
    {"@\"??_C@_0BI@PNOGLGLL@inttoptr?5?$CIi64?55?5to?5ptr?$CJ?$AA@\" = linkonce_odr dso_local unnamed_addr constant [24 x i8] c\"inttoptr (i64 5 to ptr)\\00\", comdat, align 1, !dbg !32\n",
     "@\"??_C@_0BI@PNOGLGLL@inttoptr?5?$CIi64?55?5to?5ptr?$CJ?$AA@\" = linkonce_odr dso_local unnamed_addr constant [24 x i8] c\"inttoptr (i64 5 to ptr)\\00\", comdat, align 1, !dbg !32\n", 0, 0, 0},
    {"!12 = !DIFile(filename: \"inttoptr (i64 5 to ptr)\", directory: \"C:\\\\x\")\n; inttoptr (i64 5 to ptr)\n  %x = add i32 1, 2 ; %y = inttoptr i64 %z to ptr\n",
     "!12 = !DIFile(filename: \"inttoptr (i64 5 to ptr)\", directory: \"C:\\\\x\")\n; inttoptr (i64 5 to ptr)\n  %x = add i32 1, 2 ; %y = inttoptr i64 %z to ptr\n", 0, 0, 0},
    /* 名前の一部は語として見ない */
    {"  %inttoptr = add i64 %my_inttoptr, 1\n  call void @inttoptr2()\n",
     "  %inttoptr = add i64 %my_inttoptr, 1\n  call void @inttoptr2()\n", 0, 0, 0},
    /* 知らない形は残して数える（ベクタ、値が定数式、addrspace） */
    {"  %v = inttoptr <2 x i64> %a to <2 x ptr>\n", "  %v = inttoptr <2 x i64> %a to <2 x ptr>\n", 0, 0, 1},
    {"@p = global ptr inttoptr (i64 add (i64 ptrtoint (ptr @g to i64), i64 4) to ptr)\n",
     "@p = global ptr inttoptr (i64 add (i64 ptrtoint (ptr @g to i64), i64 4) to ptr)\n", 0, 0, 1},
    {"  %q = inttoptr i64 %a to ptr addrspace(1)\n  %r = load i8, ptr addrspace(1) inttoptr (i64 5 to ptr addrspace(1))\n",
     "  %q = inttoptr i64 %a to ptr addrspace(1)\n  %r = load i8, ptr addrspace(1) inttoptr (i64 5 to ptr addrspace(1))\n", 0, 0, 2},
    /* 壊れた行と入力の終わり */
    {"", "", 0, 0, 0},
    {"inttoptr", "inttoptr", 0, 0, 1},
    {"  %1 = load i8, ptr inttoptr (", "  %1 = load i8, ptr inttoptr (", 0, 0, 1},
    {"  %1 = load i8, ptr inttoptr (i64 5 to", "  %1 = load i8, ptr inttoptr (i64 5 to", 0, 0, 1},
    {"  %1 = load i8, ptr inttoptr (i64 99999999999999999999999 to ptr)\n",
     "  %1 = load i8, ptr inttoptr (i64 99999999999999999999999 to ptr)\n", 0, 0, 1},
    {"  %8 = inttoptr i64 %", "  %8 = inttoptr i64 %", 0, 0, 1},
    {"  %8 = inttoptr i64 %\"a", "  %8 = inttoptr i64 %\"a", 0, 0, 1},
    {"  %8 = inttoptr i128 %7 to ptr\n", "  %8 = inttoptr i128 %7 to ptr\n", 0, 0, 1},
    {"@s = constant [3 x i8] c\"inttoptr", "@s = constant [3 x i8] c\"inttoptr", 0, 0, 0}
};

int main(void)
{
    size_t i;
    int failed = 0;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        sim_buf out;
        native_near_stats st;
        sim_buf_init(&out);
        native_near(cases[i].in, strlen(cases[i].in), &out, &st);
        if (strcmp(sim_buf_cstr(&out), cases[i].out)) {
            printf("ケース %zu: 出力が違います:\n%s\n", i + 1, sim_buf_cstr(&out));
            failed++;
        } else if (st.const_replaced != cases[i].cnst || st.dyn_inserted != cases[i].dyn || st.kept != cases[i].kept) {
            printf("ケース %zu: 数が違います（定数 %d、実行時 %d、残した %d）\n", i + 1,
                   st.const_replaced, st.dyn_inserted, st.kept);
            failed++;
        }
        sim_buf_free(&out);
    }
    {
        /* 長さの指定より先は読まない（文字列の途中で切った入力） */
        const char *in = "  %1 = load i8, ptr inttoptr (i64 5 to ptr)\n";
        native_near_stats st;
        sim_buf out;
        sim_buf_init(&out);
        native_near(in, 30, &out, &st);
        if (out.len != 30 || memcmp(sim_buf_cstr(&out), in, 30) || st.kept != 1) {
            puts("途中で切った入力: 出力が違います"); failed++;
        }
        sim_buf_free(&out);
    }
    printf("native near: %zu cases, %d failed\n", sizeof cases / sizeof *cases + 1, failed);
    return failed ? 1 : 0;
}
