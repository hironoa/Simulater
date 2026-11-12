/* sim_util.h - 文字列バッファ、ファイル、パスなどの小物 */
#ifndef SIM_UTIL_H
#define SIM_UTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *p;
    size_t len, cap;
} sim_buf;

void sim_buf_init(sim_buf *b);
void sim_buf_free(sim_buf *b);
void sim_buf_reserve(sim_buf *b, size_t extra);
void sim_buf_add(sim_buf *b, const void *data, size_t n);
void sim_buf_adds(sim_buf *b, const char *s);
void sim_buf_addc(sim_buf *b, char c);
#if defined(__GNUC__) || defined(__clang__)
void sim_buf_printf(sim_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void sim_buf_printf(sim_buf *b, const char *fmt, ...);
#endif
void sim_buf_vprintf(sim_buf *b, const char *fmt, va_list ap);
/* 末尾に NUL を置いた文字列として返す（len には含めない） */
const char *sim_buf_cstr(sim_buf *b);
/* JSON の文字列リテラルとして追加（引用符付き） */
void sim_buf_json_str(sim_buf *b, const char *s);
void sim_buf_json_strn(sim_buf *b, const char *s, size_t n);
/* JSON の数値（NaN/Inf は null） */
void sim_buf_json_num(sim_buf *b, double v);

void *sim_xmalloc(size_t n);
void *sim_xcalloc(size_t n, size_t m);
void *sim_xrealloc(void *p, size_t n);
char *sim_xstrdup(const char *s);
char *sim_xstrndup(const char *s, size_t n);

/* ファイル全体を読む。失敗なら NULL。末尾に NUL を付ける（len には含めない） */
char *sim_read_file(const char *path, size_t *len);
int sim_write_file(const char *path, const void *data, size_t len);

const char *sim_basename(const char *path);
/* path の末尾が suffix と（ディレクトリ区切りの境界で）一致するか。\ と / を同一視 */
int sim_path_suffix_match(const char *path, const char *suffix);
/* dir と name をつなぐ */
char *sim_path_join(const char *dir, const char *name);
int sim_mkdir_p(const char *path);

int sim_strcasestr_found(const char *hay, const char *needle);
int sim_is_ident_start(int c);
int sim_is_ident_char(int c);

/* 浮動小数を、型の精度で往復できる最短の 10 進表記にする */
void sim_format_double(char *out, size_t n, double v, int is_float32);

/* 数値の文字列を検証して読む（黙って別の値にしない）。結果は SIM_NUM_* */
enum {
    SIM_NUM_OK = 0,
    SIM_NUM_BAD = -1,    /* 数値ではない（空、空白、符号、末尾のごみ、"0x" だけ、nan / inf など） */
    SIM_NUM_RANGE = -2,  /* 範囲外（64 ビット・max を超える、double のあふれ・0 へのアンダーフロー） */
    SIM_NUM_OCTAL = -3   /* 0 で始まる 10 進（010）。C の 8 進と紛れるので受け付けない */
};
/* 符号なし整数: 10 進か 0x 16 進。文字列全体が数値で、max 以下のとき SIM_NUM_OK */
int sim_parse_uint(const char *s, uint64_t max, uint64_t *out);
int sim_parse_uint_n(const char *s, size_t n, uint64_t max, uint64_t *out);
/* 10 進の浮動小数（符号・小数点・指数可）。有限の値だけ */
int sim_parse_double(const char *s, double *out);

#endif
