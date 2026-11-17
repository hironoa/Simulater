#ifndef NATIVE_COFF_H
#define NATIVE_COFF_H

#include <stddef.h>

/* x64 の COFF オブジェクト（clang -target x86_64-pc-windows-msvc の .obj）の外部シンボルを 1 つずつ通知する。
 * name は呼び出しの間だけ有効。弱い外部は通知しない。成功は 0、読めない形式（bigobj、別の機械、壊れている）は -1。
 * is_function は Type の派生型が関数かどうか。clang は未定義のシンボルに関数の印を付けない（実物の .obj で
 * 確かめた）ので、未定義のシンボルでは当てにならない。関数か変数かは native_ll_decls で宣言から決める。 */
typedef void (*native_coff_visit)(void *ctx, const char *name, int defined, int is_function);
int native_coff_symbols(const unsigned char *data, size_t len, native_coff_visit visit,
                        void *ctx, char *err, size_t errlen);

/* LLVM IR のテキスト（clang -S -emit-llvm の .ll）から、関数の宣言（declare の行）と、定義のない外部の変数
 * （@名前 = external / extern_weak … global / constant 型）を 1 つずつ通知する。name は呼び出しの間だけ有効。
 * size は変数の大きさ（バイト）。iN、ptr、float、double と、それらの配列だけ求める。それ以外と大きさ 0 は 0（不明）。
 * 関数では 0。ret は関数の戻り値の種類（NATIVE_RET_…）。変数では NATIVE_RET_INT。 */
enum {
    NATIVE_RET_INT,     /* void、整数（iN）、ポインタ: RAX で返る */
    NATIVE_RET_F32,     /* float: XMM0 */
    NATIVE_RET_F64,     /* double: XMM0 */
    NATIVE_RET_OTHER,   /* それ以外（x86_fp80、ベクタ、構造体の値など）。代わりは正しく作れない */
    NATIVE_RET_SRET     /* 構造体を隠れた第 1 引数の置き場所に返す（sret）。代わりは戻り値を書かない */
};
#define NATIVE_ALIGN_MAX 4096UL
/* align は変数の宣言の整列（型のあとの `, align N`）。ないもの、2 の累乗でないもの、NATIVE_ALIGN_MAX を超えるものは 0（不明）。
 * 関数では 0 */
typedef void (*native_ll_visit)(void *ctx, const char *name, int is_function, unsigned long size, int ret,
                                unsigned long align);
void native_ll_decls(const char *text, size_t len, native_ll_visit visit, void *ctx);

#endif
