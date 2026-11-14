/* sim_json.h - 小さな JSON パーサ（DOM）。書き出しは sim_buf_json_* を使う */
#ifndef SIM_JSON_H
#define SIM_JSON_H

#include <stddef.h>

typedef enum { SJ_NULL, SJ_BOOL, SJ_NUM, SJ_STR, SJ_ARR, SJ_OBJ } sj_type;

typedef struct sj_value sj_value;
struct sj_value {
    sj_type type;
    double num;          /* SJ_NUM、SJ_BOOL(0/1) */
    char *str;           /* SJ_STR（NUL 終端。途中に NUL を含む場合は len を使う） */
    size_t len;
    char *key;           /* オブジェクトのメンバならキー */
    sj_value *child;     /* SJ_ARR / SJ_OBJ の最初の子 */
    sj_value *next;
    size_t nchild;
    const char *raw;     /* SJ_NUM の元の文字列（入力内を指す） */
    size_t rawlen;
};

/* 失敗なら NULL と err（位置付き） */
sj_value *sj_parse(const char *text, size_t len, char *err, size_t errlen);
void sj_free(sj_value *v);
sj_value *sj_get(const sj_value *obj, const char *key);
const char *sj_get_str(const sj_value *obj, const char *key, const char *def);
double sj_get_num(const sj_value *obj, const char *key, double def);
int sj_get_bool(const sj_value *obj, const char *key, int def);

#endif
