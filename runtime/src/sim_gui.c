#define _CRT_SECURE_NO_WARNINGS
#include "sim_gui.h"
#include "sim_csv.h"
#include "sim_json.h"
#include "sim_platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_VERSION "0.1.0"
#define SSE_INTERVAL_MS 50
#define SSE_MAX_ROWS 2000

/* ================================================================ 状態 */

typedef struct { char *name; sim_ref *ref; int col; } watch_t;
typedef struct { char *name; sim_ref *ref; sim_cellval val; char *text; int col; } hold_t;
typedef struct { char *name; sim_ref *ref; sim_cellval val; char *text; int col; } pend_t;
/* v = 出力 nout 個の数値（SSE の samples 用。読めなければ NAN）。
 * txt = 出力 nout 個 + 入力 nin 個のセルの文字列を NUL 区切りで並べたもの（空文字列は値なし）。
 * エクスポートはこの文字列を使う。入力は書いたセルの元の文字列（'XYZ'、1|2|3、0x78 など）、
 * 出力は sim run が結果 CSV に書くのと同じ表記（配列は全要素）なので、保存した CSV を再実行すると同じ結果になる */
typedef struct { uint64_t step; int nout, nin; double *v; char *txt; } rec_row;

typedef struct {
    sim_ctx *c;
    sim_backend *b;
    sim_mutex *mu;
    sim_cond *cv;

    int running;
    uint64_t step;
    uint64_t budget;          /* step コマンドの残り */
    double speed;
    double period_ms;
    int overrun;
    char func[256];
    char error[1024];
    uint64_t state_ver;
    uint64_t values_ver;      /* 一時停止中の現在値（event: values）を送り直す合図 */

    watch_t *watch;
    int nwatch;
    uint64_t watch_ver;
    hold_t *holds;
    int nholds;
    pend_t *pend;
    int npend;
    /* 関数の変更などで解決し直せずに外した固定・監視（直近の /api/control・/api/load-csv の分）。
     * JSON の {"name","kind","message"} を ',' で並べたもの。state の "dropped" に載せる */
    sim_buf dropped;

    sim_csv_table play;
    sim_ref **play_refs;
    int play_cols;            /* play_refs の数 */
    int *play_col_ids;
    int play_active;
    int play_pos;

    char **out_cols;
    int nout_cols;
    char **in_cols;
    int nin_cols;
    rec_row *ring;
    size_t cap;
    uint64_t total;
    uint64_t epoch;

    char html_path[4096];
    int port;
} gui_t;

static gui_t G;

static void state_changed(void) { G.state_ver++; sim_cond_broadcast(G.cv); }
static void values_changed(void) { G.values_ver++; sim_cond_broadcast(G.cv); }

static int col_index(char ***cols, int *n, const char *name)
{
    int i;
    for (i = 0; i < *n; i++) if (strcmp((*cols)[i], name) == 0) return i;
    *cols = sim_xrealloc(*cols, (size_t)(*n + 1) * sizeof **cols);
    (*cols)[*n] = sim_xstrdup(name);
    return (*n)++;
}

static void rec_clear(void)
{
    size_t i;
    for (i = 0; i < G.cap; i++) {
        free(G.ring[i].v);
        free(G.ring[i].txt);
        G.ring[i].v = NULL;
        G.ring[i].txt = NULL;
    }
    G.total = 0;
    G.epoch++;
}

/* in = 入力 nin 個のセルの文字列（NULL は値なし） */
static void rec_push(uint64_t step, char *const *in, int nin)
{
    rec_row *r = &G.ring[G.total % G.cap];
    char **out;
    sim_buf tb;
    int i;
    free(r->v);
    free(r->txt);
    r->step = step;
    r->nout = G.nout_cols;
    r->nin = nin;
    r->v = sim_xmalloc(sizeof(double) * (size_t)(r->nout + 1));
    out = sim_xcalloc((size_t)r->nout + 1, sizeof *out);
    for (i = 0; i < r->nout; i++) r->v[i] = NAN;
    for (i = 0; i < G.nwatch; i++) {
        double d;
        int col = G.watch[i].col, match;
        char err[256];
        sim_buf cb;
        if (col >= r->nout) continue;
        if (sim_ref_read_double(G.c, G.watch[i].ref, &d) == 0) r->v[col] = d;
        sim_buf_init(&cb);
        if (sim_ref_check(G.c, G.watch[i].ref, NULL, NULL, &cb, &match, err, sizeof err) == 0 && cb.len) {
            free(out[col]);
            out[col] = cb.p;   /* 持ち主を移す */
        } else {
            sim_buf_free(&cb);
        }
    }
    sim_buf_init(&tb);
    for (i = 0; i < r->nout; i++) {
        if (out[i]) sim_buf_adds(&tb, out[i]);
        sim_buf_addc(&tb, 0);
        free(out[i]);
    }
    free(out);
    for (i = 0; i < nin; i++) {
        if (in[i]) sim_buf_adds(&tb, in[i]);
        sim_buf_addc(&tb, 0);
    }
    r->txt = tb.p;   /* 列が 0 個なら NULL */
    G.total++;
}

/* 記録のセルを idx に並べる（出力 0..nout-1、入力 nout..）。値がなければ "" */
static void rec_cells(const rec_row *r, const char **idx)
{
    const char *p = r->txt;
    int k;
    for (k = 0; k < r->nout + r->nin; k++) {
        idx[k] = p ? p : "";
        if (p) p += strlen(p) + 1;
    }
}

/* 入力の記録: in[col] を text に置き換える（同じステップで同じ列に何度も書いたら最後の値） */
static void rec_set_in(char **in, int *nin, int col, const char *text)
{
    if (col < 0) return;
    free(in[col]);
    in[col] = sim_xstrdup(text);
    if (col + 1 > *nin) *nin = col + 1;
}

static double cell_first(const sim_cellval *v)
{
    if (v->n != 1 || v->is_string) return NAN;
    if (v->items[0].kind == SIM_ITEM_FLOAT) return v->items[0].f;
    return (double)v->items[0].i;
}

/* ================================================================ 周期実行 */

static void set_error(const char *msg)
{
    snprintf(G.error, sizeof G.error, "%s", msg);
    fprintf(stderr, "sim gui: %s\n", msg);
    state_changed();
}

static void free_pend(pend_t *p)
{
    free(p->name);
    free(p->text);
    sim_ref_free(p->ref);
    sim_cellval_free(&p->val);
}

/* 1 ステップの入力 1 個（単発書き込み・固定・CSV の再生） */
typedef struct {
    int col;                  /* 記録の入力列（エクスポートの入力列の順） */
    int seq;                  /* 集めた順（同じ列の中では後のものが勝つ） */
    sim_ref *ref;
    const sim_cellval *val;
    const char *text;
    int play_row;             /* CSV の再生なら行番号（1 から）、違えば 0 */
} step_in_t;

static int step_in_cmp(const void *a, const void *b)
{
    const step_in_t *x = a, *y = b;
    if (x->col != y->col) return x->col < y->col ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq);
}

/* 1 ステップの入力を書き込み、書いたセルの元の文字列を in[列] に記録する。
 * 保存した CSV を sim run で再実行したときと同じになるように、sim run が 1 行の入力列を左から順に 1 回ずつ
 * 書くのに合わせる:
 *   - 単発書き込み・固定・CSV の再生をまとめて、入力列の番号（エクスポートの列の順）の昇順に書く。
 *     受け付けた順や種類の順ではない（P1 の固定と P1_bit.no0 の単発のように重なる先があると結果が変わる）
 *   - 同じ列（同じ名前）への書き込みが何個かあれば、最後のもの（単発 < 固定 < 再生、単発どうしは受け付けた順）
 *     だけを書く。記録は 1 列 1 セルなので、前のものも書くと '$' の確保が二重になる・長い文字列の残りが残るなど、
 *     再実行と合わなくなる
 *   - 空の値（空セル）は書かず、記録もしない */
static void apply_inputs_locked(char **in, int *nin)
{
    char err[1024];
    int i, j, n = 0, cap = G.npend + G.nholds + (G.play_active ? G.play.nin : 0) + 1;
    step_in_t *v = sim_xcalloc((size_t)cap, sizeof *v);
    sim_cellval *pv = sim_xcalloc((size_t)(G.play_active ? G.play.nin : 0) + 1, sizeof *pv);
    for (i = 0; i < G.npend; i++) {
        pend_t *p = &G.pend[i];
        if (!sim_cellval_present(&p->val)) continue;
        v[n].col = p->col; v[n].seq = n; v[n].ref = p->ref; v[n].val = &p->val; v[n].text = p->text; n++;
    }
    for (i = 0; i < G.nholds; i++) {
        hold_t *h = &G.holds[i];
        if (!sim_cellval_present(&h->val)) continue;
        v[n].col = h->col; v[n].seq = n; v[n].ref = h->ref; v[n].val = &h->val; v[n].text = h->text; n++;
    }
    if (G.play_active && G.play_pos < G.play.nrows) {
        char **row = G.play.cells[G.play_pos];
        for (i = 0; i < G.play.nin; i++) {
            if (!row[i] || !G.play_refs[i]) continue;
            if (sim_parse_cell(row[i], strlen(row[i]), &pv[i], err, sizeof err)) {
                char m[1200];
                snprintf(m, sizeof m, "CSV %d 行目のデータ: %s", G.play_pos + 1, err);
                set_error(m);
                continue;
            }
            if (!sim_cellval_present(&pv[i])) continue;
            v[n].col = G.play_col_ids[i]; v[n].seq = n; v[n].ref = G.play_refs[i]; v[n].val = &pv[i];
            v[n].text = row[i]; v[n].play_row = G.play_pos + 1; n++;
        }
        G.play_pos++;
    }
    qsort(v, (size_t)n, sizeof *v, step_in_cmp);
    for (i = 0; i < n; i = j) {
        step_in_t *w;
        for (j = i + 1; j < n && v[j].col == v[i].col; j++) {}
        w = &v[j - 1];   /* 同じ列の最後のもの */
        if (sim_ref_write(G.c, w->ref, w->val, err, sizeof err)) {
            if (w->play_row) {
                char m[1200];
                snprintf(m, sizeof m, "CSV %d 行目のデータ: %s", w->play_row, err);
                set_error(m);
            } else {
                set_error(err);
            }
        } else {
            rec_set_in(in, nin, w->col, w->text);
        }
    }
    free(v);
    for (i = 0; i < (G.play_active ? G.play.nin : 0); i++) sim_cellval_free(&pv[i]);
    free(pv);
    for (i = 0; i < G.npend; i++) free_pend(&G.pend[i]);
    G.npend = 0;
}

static void do_step_locked(void)
{
    char err[1024];
    char **in = sim_xcalloc((size_t)G.nin_cols + 1, sizeof *in);
    int i, nin = 0;
    apply_inputs_locked(in, &nin);
    if (sim_ctx_func(G.c)) {
        if (sim_ctx_call(G.c, err, sizeof err)) { set_error(err); G.running = 0; G.budget = 0; }
    }
    G.step++;
    rec_push(G.step, in, nin);
    for (i = 0; i < G.nin_cols; i++) free(in[i]);
    free(in);
    if (G.play_active && G.play_pos >= G.play.nrows) {
        G.play_active = 0;
        G.running = 0;
        state_changed();
    }
}

static void sim_loop(void *arg)
{
    uint64_t next = sim_now_ns(), last_overrun = 0, last_yield = sim_now_ns();
    (void)arg;
    sim_mutex_lock(G.mu);
    for (;;) {
        if (G.budget > 0) {
            do_step_locked();
            if (G.budget > 0) G.budget--;
            if (G.budget == 0) state_changed();
            sim_mutex_unlock(G.mu);
            sim_mutex_lock(G.mu);
            next = sim_now_ns();
            continue;
        }
        if (!G.running) {
            /* 止まっている間の書き込みは api_write がすぐ反映済み。待ち行列は次のステップで入力として記録する */
            sim_cond_wait(G.cv, G.mu, 50);
            next = sim_now_ns();
            continue;
        }
        do_step_locked();
        if (G.speed > 0) {
            uint64_t interval = (uint64_t)(G.period_ms / G.speed * 1e6);
            uint64_t now = sim_now_ns();
            next += interval;
            if (now > next + interval) {
                /* 追いつけない: 周期を詰めずに続ける */
                if (!G.overrun) { G.overrun = 1; state_changed(); }
                last_overrun = now;
                next = now;
            } else if (G.overrun && now - last_overrun > 1000000000ULL) {
                G.overrun = 0;
                state_changed();
            }
            sim_mutex_unlock(G.mu);
            sim_sleep_until_ns(next);
            sim_mutex_lock(G.mu);
        } else {
            uint64_t now = sim_now_ns();
            if (G.overrun) { G.overrun = 0; state_changed(); }
            if (now - last_yield > 5000000ULL) {
                sim_mutex_unlock(G.mu);
                sim_sleep_ms(1);
                sim_mutex_lock(G.mu);
                last_yield = sim_now_ns();
            } else {
                sim_mutex_unlock(G.mu);
                sim_mutex_lock(G.mu);
            }
            next = sim_now_ns();
        }
    }
}

/* ================================================================ JSON の部品 */

static void json_holds(sim_buf *b)
{
    int i;
    sim_buf_addc(b, '[');
    for (i = 0; i < G.nholds; i++) {
        double d = cell_first(&G.holds[i].val);
        if (i) sim_buf_addc(b, ',');
        sim_buf_adds(b, "{\"name\":");
        sim_buf_json_str(b, G.holds[i].name);
        sim_buf_adds(b, ",\"value\":");
        if (!isnan(d)) sim_buf_json_num(b, d);
        else sim_buf_json_str(b, G.holds[i].text);
        sim_buf_addc(b, '}');
    }
    sim_buf_addc(b, ']');
}

static void json_state(sim_buf *b)
{
    sim_buf_printf(b, "{\"running\":%s,\"step\":%llu,\"speed\":", G.running || G.budget ? "true" : "false",
                   (unsigned long long)G.step);
    sim_buf_json_num(b, G.speed);
    sim_buf_adds(b, ",\"holds\":");
    json_holds(b);
    sim_buf_printf(b, ",\"overrun\":%s,\"periodMs\":", G.overrun ? "true" : "false");
    sim_buf_json_num(b, G.period_ms);
    sim_buf_adds(b, ",\"func\":");
    sim_buf_json_str(b, G.func);
    sim_buf_printf(b, ",\"playback\":%s,\"error\":", G.play_active ? "true" : "false");
    sim_buf_json_str(b, G.error);
    sim_buf_adds(b, ",\"dropped\":[");
    sim_buf_add(b, G.dropped.p ? G.dropped.p : "", G.dropped.len);
    sim_buf_adds(b, "]}");
}

static void json_watch_names(sim_buf *b)
{
    int i;
    sim_buf_addc(b, '[');
    for (i = 0; i < G.nwatch; i++) {
        if (i) sim_buf_addc(b, ',');
        sim_buf_json_str(b, G.watch[i].name);
    }
    sim_buf_addc(b, ']');
}

/* 監視変数の現在値（ステップは進めない）。読めなければ null */
static void json_values(sim_buf *b)
{
    int i;
    sim_buf_printf(b, "{\"step\":%llu,\"names\":", (unsigned long long)G.step);
    json_watch_names(b);
    sim_buf_adds(b, ",\"values\":[");
    for (i = 0; i < G.nwatch; i++) {
        double d;
        if (i) sim_buf_addc(b, ',');
        if (sim_ref_read_double(G.c, G.watch[i].ref, &d) == 0) sim_buf_json_num(b, d);
        else sim_buf_adds(b, "null");
    }
    sim_buf_adds(b, "]}");
}

static void json_hello(sim_buf *b)
{
    sim_buf_adds(b, "{\"periodMs\":");
    sim_buf_json_num(b, G.period_ms);
    sim_buf_adds(b, ",\"func\":");
    sim_buf_json_str(b, G.func);
    sim_buf_adds(b, ",\"names\":");
    json_watch_names(b);
    sim_buf_addc(b, '}');
}

/* ================================================================ HTTP */

typedef struct {
    char method[16];
    char path[2048];
    char query[4096];
    char *body;
    size_t body_len;
} req_t;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *s, size_t n, char *out, size_t on)
{
    size_t i, k = 0;
    for (i = 0; i < n && k + 1 < on; i++) {
        if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out[k++] = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') out[k++] = ' ';
        else out[k++] = s[i];
    }
    out[k] = 0;
}

static int query_param(const char *q, const char *key, char *out, size_t on)
{
    size_t kl = strlen(key);
    const char *p = q;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg >= kl + 1 && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            url_decode(p + kl + 1, seg - kl - 1, out, on);
            return 1;
        }
        if (seg == kl && strncmp(p, key, kl) == 0) { out[0] = 0; return 1; }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    default: return "Internal Server Error";
    }
}

static void send_resp(sim_sock s, int code, const char *ctype, const char *body, size_t n, const char *extra)
{
    sim_buf h;
    sim_buf_init(&h);
    sim_buf_printf(&h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                       "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                       "Access-Control-Allow-Headers: Content-Type\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                       "%sConnection: close\r\n\r\n",
                   code, status_text(code), ctype, n, extra ? extra : "");
    sim_send_all(s, h.p, h.len);
    if (n) sim_send_all(s, body, n);
    sim_buf_free(&h);
}

static void send_json(sim_sock s, int code, sim_buf *b)
{
    send_resp(s, code, "application/json; charset=utf-8", b->p ? b->p : "", b->len, NULL);
}

static void send_error(sim_sock s, int code, const char *msg)
{
    sim_buf b;
    sim_buf_init(&b);
    sim_buf_adds(&b, "{\"error\":");
    sim_buf_json_str(&b, msg);
    sim_buf_addc(&b, '}');
    send_json(s, code, &b);
    sim_buf_free(&b);
}

static int read_request(sim_sock s, req_t *r)
{
    sim_buf b;
    char tmp[8192];
    char *hend = NULL, *line, *p;
    size_t clen = 0, hlen;
    memset(r, 0, sizeof *r);
    sim_buf_init(&b);
    while (!hend) {
        long n = sim_recv(s, tmp, sizeof tmp);
        if (n <= 0) { sim_buf_free(&b); return -1; }
        sim_buf_add(&b, tmp, (size_t)n);
        hend = strstr(b.p, "\r\n\r\n");
        if (!hend && b.len > 65536) { sim_buf_free(&b); return -1; }
    }
    hlen = (size_t)(hend - b.p) + 4;
    /* 要求行 */
    line = b.p;
    p = strchr(line, ' ');
    if (!p) { sim_buf_free(&b); return -1; }
    snprintf(r->method, sizeof r->method, "%.*s", (int)(p - line), line);
    line = p + 1;
    p = strchr(line, ' ');
    if (!p) { sim_buf_free(&b); return -1; }
    {
        size_t ul = (size_t)(p - line);
        char *qm = memchr(line, '?', ul);
        if (qm) {
            snprintf(r->path, sizeof r->path, "%.*s", (int)(qm - line), line);
            snprintf(r->query, sizeof r->query, "%.*s", (int)(ul - (size_t)(qm - line) - 1), qm + 1);
        } else {
            snprintf(r->path, sizeof r->path, "%.*s", (int)ul, line);
        }
    }
    /* Content-Length */
    for (p = b.p; p < hend; ) {
        char *eol = strstr(p, "\r\n");
        if (!eol) break;
        if (eol - p > 15) {
            char key[16];
            int k;
            for (k = 0; k < 15; k++) key[k] = (char)((p[k] >= 'A' && p[k] <= 'Z') ? p[k] + 32 : p[k]);
            key[15] = 0;
            if (strncmp(key, "content-length:", 15) == 0) clen = (size_t)strtoull(p + 15, NULL, 10);
        }
        p = eol + 2;
    }
    if (clen > 64u * 1024u * 1024u) { sim_buf_free(&b); return -2; }
    while (b.len - hlen < clen) {
        long n = sim_recv(s, tmp, sizeof tmp);
        if (n <= 0) { sim_buf_free(&b); return -1; }
        sim_buf_add(&b, tmp, (size_t)n);
    }
    r->body = sim_xmalloc(clen + 1);
    memcpy(r->body, b.p + hlen, clen);
    r->body[clen] = 0;
    r->body_len = clen;
    sim_buf_free(&b);
    return 0;
}

/* ================================================================ API */

static void api_info(sim_sock s)
{
    sim_buf b;
    size_t i, n = sim_ctx_backend(G.c)->func_count(sim_ctx_backend(G.c));
    sim_buf_init(&b);
    sim_mutex_lock(G.mu);
    sim_buf_adds(&b, "{\"version\":\"" SIM_VERSION "\",\"backend\":");
    sim_buf_json_str(&b, G.b->name);
    sim_buf_adds(&b, ",\"periodMs\":");
    sim_buf_json_num(&b, G.period_ms);
    sim_buf_adds(&b, ",\"func\":");
    sim_buf_json_str(&b, G.func);
    sim_buf_adds(&b, ",\"functions\":[");
    for (i = 0; i < n; i++) {
        if (i) sim_buf_addc(&b, ',');
        sim_buf_json_str(&b, G.b->func_at(G.b, i)->name);
    }
    sim_buf_adds(&b, "],\"state\":");
    json_state(&b);
    sim_buf_addc(&b, '}');
    sim_mutex_unlock(G.mu);
    send_json(s, 200, &b);
    sim_buf_free(&b);
}

typedef struct {
    sim_buf *out;
    const char *q;
    int limit, count;
    long visited;
} search_t;

static int scalar_t(const sim_type *t)
{
    return t && (t->kind == SIM_T_INT || t->kind == SIM_T_UINT || t->kind == SIM_T_BOOL ||
                 t->kind == SIM_T_FLOAT || t->kind == SIM_T_PTR);
}

static void child_names(const sim_type *t, const char *name, sim_buf *b, int *first)
{
    int i;
    if (!t) return;
    if (t->kind == SIM_T_STRUCT || t->kind == SIM_T_UNION) {
        for (i = 0; i < t->nmembers; i++) {
            const sim_member *m = &t->members[i];
            if (!m->name || !m->name[0]) { child_names(m->type, name, b, first); continue; }
            if (!*first) sim_buf_addc(b, ',');
            *first = 0;
            {
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%s.%s", name, m->name);
                sim_buf_json_str(b, tmp);
            }
        }
    } else if (t->kind == SIM_T_ARRAY) {
        uint32_t k, n = t->count > 256 ? 256 : t->count;
        for (k = 0; k < n; k++) {
            char tmp[1024];
            if (!*first) sim_buf_addc(b, ',');
            *first = 0;
            snprintf(tmp, sizeof tmp, "%s[%u]", name, k);
            sim_buf_json_str(b, tmp);
        }
    }
}

static void search_rec(search_t *st, const char *name, const sim_type *t, int is_const, int depth)
{
    char tn[256];
    int i;
    if (st->count >= st->limit || st->visited > 500000 || depth > 8) return;
    st->visited++;
    if (sim_strcasestr_found(name, st->q)) {
        int first = 1;
        if (st->count) sim_buf_addc(st->out, ',');
        st->count++;
        sim_type_display(t, tn, sizeof tn);
        sim_buf_adds(st->out, "{\"name\":");
        sim_buf_json_str(st->out, name);
        sim_buf_adds(st->out, ",\"type\":");
        sim_buf_json_str(st->out, tn);
        sim_buf_printf(st->out, ",\"size\":%u,\"kind\":\"%s\",\"writable\":%s,\"members\":[", t ? t->size : 0,
                       sim_type_kind_name(t), (!is_const && scalar_t(t)) ? "true" : "false");
        child_names(t, name, st->out, &first);
        sim_buf_adds(st->out, "]}");
    }
    if (!t) return;
    if (t->kind == SIM_T_STRUCT || t->kind == SIM_T_UNION) {
        for (i = 0; i < t->nmembers; i++) {
            const sim_member *m = &t->members[i];
            char tmp[1024];
            if (!m->name || !m->name[0]) {
                /* 匿名メンバは親の名前のまま中を見る */
                const sim_type *at = m->type;
                int k;
                if (at && (at->kind == SIM_T_STRUCT || at->kind == SIM_T_UNION))
                    for (k = 0; k < at->nmembers; k++) {
                        const sim_member *m2 = &at->members[k];
                        if (!m2->name || !m2->name[0]) continue;
                        snprintf(tmp, sizeof tmp, "%s.%s", name, m2->name);
                        search_rec(st, tmp, m2->type, is_const, depth + 1);
                    }
                continue;
            }
            snprintf(tmp, sizeof tmp, "%s.%s", name, m->name);
            search_rec(st, tmp, m->type, is_const, depth + 1);
        }
    } else if (t->kind == SIM_T_ARRAY) {
        uint32_t k, n = t->count > 256 ? 256 : t->count;
        for (k = 0; k < n; k++) {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s[%u]", name, k);
            search_rec(st, tmp, t->target, is_const, depth + 1);
        }
    }
}

static void api_symbols(sim_sock s, req_t *r)
{
    char q[512] = "", lim[32] = "";
    search_t st;
    sim_buf b;
    size_t i, n;
    const sim_func *f;
    query_param(r->query, "q", q, sizeof q);
    st.limit = 200;
    if (query_param(r->query, "limit", lim, sizeof lim) && atoi(lim) > 0) st.limit = atoi(lim);
    if (st.limit > 5000) st.limit = 5000;
    st.q = q;
    st.count = 0;
    st.visited = 0;
    sim_buf_init(&b);
    st.out = &b;
    sim_buf_addc(&b, '[');
    sim_mutex_lock(G.mu);
    f = sim_ctx_func(G.c);
    if (f) {
        int k;
        for (k = 0; k < f->nparams; k++) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "@%s", f->param_names[k] ? f->param_names[k] : "?");
            search_rec(&st, tmp, f->param_types[k], 0, 0);
        }
        if (f->ret && f->ret->kind != SIM_T_VOID) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s@@", f->name);
            search_rec(&st, tmp, f->ret, 1, 0);
        }
    }
    n = sim_ctx_sym_count(G.c);
    for (i = 0; i < n && st.count < st.limit; i++)
        search_rec(&st, sim_ctx_sym_name(G.c, i), sim_ctx_sym_type(G.c, i), sim_ctx_sym_const(G.c, i), 0);
    sim_mutex_unlock(G.mu);
    sim_buf_addc(&b, ']');
    send_json(s, 200, &b);
    sim_buf_free(&b);
}

static void free_watch(void)
{
    int i;
    for (i = 0; i < G.nwatch; i++) { free(G.watch[i].name); sim_ref_free(G.watch[i].ref); }
    free(G.watch);
    G.watch = NULL;
    G.nwatch = 0;
}

/* 名前の一覧を監視に設定（append なら追加）。errors に結果を書く */
static void set_watch_locked(const char *const *names, int n, int append, sim_buf *errs)
{
    int i, j;
    char err[1024];
    int first = 1;
    if (!append) free_watch();
    for (i = 0; i < n; i++) {
        sim_ref *r;
        int dup = 0;
        for (j = 0; j < G.nwatch; j++) if (strcmp(G.watch[j].name, names[i]) == 0) dup = 1;
        if (dup) continue;
        r = sim_ref_parse(G.c, names[i], err, sizeof err);
        if (!r || sim_ref_prefix(r) == '$') {
            if (r) { snprintf(err, sizeof err, "'%s': '$' は監視できません", names[i]); sim_ref_free(r); }
            if (errs) {
                if (!first) sim_buf_addc(errs, ',');
                first = 0;
                sim_buf_adds(errs, "{\"name\":");
                sim_buf_json_str(errs, names[i]);
                sim_buf_adds(errs, ",\"message\":");
                sim_buf_json_str(errs, err);
                sim_buf_addc(errs, '}');
            }
            continue;
        }
        G.watch = sim_xrealloc(G.watch, (size_t)(G.nwatch + 1) * sizeof *G.watch);
        G.watch[G.nwatch].name = sim_xstrdup(names[i]);
        G.watch[G.nwatch].ref = r;
        G.watch[G.nwatch].col = col_index(&G.out_cols, &G.nout_cols, names[i]);
        G.nwatch++;
    }
    G.watch_ver++;
    values_changed();
}

static sj_value *parse_body(sim_sock s, req_t *r)
{
    char err[256];
    sj_value *v = sj_parse(r->body, r->body_len, err, sizeof err);
    if (!v || v->type != SJ_OBJ) {
        send_error(s, 400, v ? "本文は JSON のオブジェクトにしてください" : err);
        sj_free(v);
        return NULL;
    }
    return v;
}

static void api_watch(sim_sock s, req_t *r)
{
    sj_value *v = parse_body(s, r), *arr, *it;
    const char **names;
    int n = 0;
    sim_buf b, errs;
    if (!v) return;
    arr = sj_get(v, "names");
    if (!arr || arr->type != SJ_ARR) { send_error(s, 400, "\"names\" に変数名の配列を指定してください"); sj_free(v); return; }
    names = sim_xcalloc(arr->nchild + 1, sizeof *names);
    for (it = arr->child; it; it = it->next) if (it->type == SJ_STR) names[n++] = it->str;
    sim_buf_init(&b);
    sim_buf_init(&errs);
    sim_mutex_lock(G.mu);
    set_watch_locked(names, n, 0, &errs);
    sim_buf_adds(&b, "{\"names\":");
    json_watch_names(&b);
    sim_buf_adds(&b, ",\"errors\":[");
    sim_buf_add(&b, errs.p ? errs.p : "", errs.len);
    sim_buf_adds(&b, "]}");
    sim_mutex_unlock(G.mu);
    send_json(s, 200, &b);
    sim_buf_free(&b);
    sim_buf_free(&errs);
    free(names);
    sj_free(v);
}

static void free_hold(hold_t *h)
{
    free(h->name);
    free(h->text);
    sim_ref_free(h->ref);
    sim_cellval_free(&h->val);
}

static void api_write(sim_sock s, req_t *r)
{
    sj_value *v = parse_body(s, r), *val;
    const char *name;
    char err[1024], num[64];
    const char *text = num;   /* 文字列はそのまま使う（長さで切り詰めない） */
    sim_ref *ref;
    sim_cellval cv;
    int hold, release, i, written_now;
    if (!v) return;
    name = sj_get_str(v, "name", NULL);
    if (!name) { send_error(s, 400, "\"name\" がありません"); sj_free(v); return; }
    release = sj_get_bool(v, "release", 0);
    hold = sj_get_bool(v, "hold", 0);
    sim_mutex_lock(G.mu);
    if (release) {
        for (i = 0; i < G.nholds; i++) {
            if (strcmp(G.holds[i].name, name) == 0) {
                free_hold(&G.holds[i]);
                memmove(&G.holds[i], &G.holds[i + 1], (size_t)(G.nholds - i - 1) * sizeof *G.holds);
                G.nholds--;
                break;
            }
        }
        state_changed();
        sim_mutex_unlock(G.mu);
        send_resp(s, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, NULL);
        sj_free(v);
        return;
    }
    val = sj_get(v, "value");
    if (!val || (val->type != SJ_NUM && val->type != SJ_STR && val->type != SJ_BOOL)) {
        sim_mutex_unlock(G.mu);
        send_error(s, 400, "\"value\" に数値か文字列を指定してください");
        sj_free(v);
        return;
    }
    if (val->type == SJ_STR) text = val->str;
    else if (val->type == SJ_BOOL) snprintf(num, sizeof num, "%d", val->num != 0);
    else if (val->num == floor(val->num) && fabs(val->num) < 9e18) snprintf(num, sizeof num, "%lld", (long long)val->num);
    else snprintf(num, sizeof num, "%.17g", val->num);
    ref = sim_ref_parse(G.c, name, err, sizeof err);
    if (!ref) { sim_mutex_unlock(G.mu); send_error(s, 400, err); sj_free(v); return; }
    if (!sim_ref_writable(ref)) {
        sim_ref_free(ref);
        sim_mutex_unlock(G.mu);
        snprintf(err, sizeof err, "'%s' には書き込めません", name);
        send_error(s, 400, err);
        sj_free(v);
        return;
    }
    if (strchr(text, '\r') || strchr(text, '\n')) {
        /* 記録を CSV に保存すると行が割れて再実行できないので受け付けない */
        sim_ref_free(ref);
        sim_mutex_unlock(G.mu);
        send_error(s, 400, "値に改行は使えません");
        sj_free(v);
        return;
    }
    if (sim_parse_cell(text, strlen(text), &cv, err, sizeof err)) {
        sim_ref_free(ref);
        sim_mutex_unlock(G.mu);
        send_error(s, 400, err);
        sj_free(v);
        return;
    }
    if (sim_ref_prefix(ref) == '$' && sim_cellval_present(&cv) &&
        (cv.n != 1 || cv.is_string || cv.items[0].kind != SIM_ITEM_INT || cv.items[0].i < 0)) {
        snprintf(err, sizeof err, "'%s': '$' の値は確保する要素数（0 以上の整数）です", name);
        sim_ref_free(ref);
        sim_cellval_free(&cv);
        sim_mutex_unlock(G.mu);
        send_error(s, 400, err);
        sj_free(v);
        return;
    }
    written_now = 0;
    if (!G.running && !G.budget && sim_ref_prefix(ref) != '$') {
        /* 止まっていればすぐ書く。'$'（確保）は次のステップで 1 回だけ書く（すぐ書いてからもう一度書くと
         * 二重に確保してポインタが変わり、保存した CSV の再実行と合わなくなる） */
        written_now = 1;
        if (sim_ref_write(G.c, ref, &cv, err, sizeof err)) {
            sim_ref_free(ref);
            sim_cellval_free(&cv);
            sim_mutex_unlock(G.mu);
            send_error(s, 400, err);
            sj_free(v);
            return;
        }
        values_changed();
    }
    if (!hold || written_now) {
        /* 単発書き込みは次のステップの入力として書いて記録する。止まっているときにすぐ書いた値は、固定でも
         * 待ち行列に入れる（同じ値をもう一度書く）。次のステップまでに固定を外しても、すぐ書いた値が記録に残り、
         * 保存した CSV の再実行と合う */
        sim_cellval pc = cv;
        sim_ref *pr = ref;
        if (hold) {
            /* 固定とは別に持つ（上で解析・解決できた文字列と名前なので失敗しない） */
            pr = sim_ref_parse(G.c, name, err, sizeof err);
            if (pr && sim_parse_cell(text, strlen(text), &pc, err, sizeof err)) { sim_ref_free(pr); pr = NULL; }
        }
        if (pr) {
            G.pend = sim_xrealloc(G.pend, (size_t)(G.npend + 1) * sizeof *G.pend);
            G.pend[G.npend].name = sim_xstrdup(name);
            G.pend[G.npend].text = sim_xstrdup(text);
            G.pend[G.npend].ref = pr;
            G.pend[G.npend].val = pc;
            G.pend[G.npend].col = col_index(&G.in_cols, &G.nin_cols, name);
            G.npend++;
        }
    }
    if (hold) {
        hold_t *h = NULL;
        for (i = 0; i < G.nholds; i++) if (strcmp(G.holds[i].name, name) == 0) { h = &G.holds[i]; free_hold(h); break; }
        if (!h) {
            G.holds = sim_xrealloc(G.holds, (size_t)(G.nholds + 1) * sizeof *G.holds);
            h = &G.holds[G.nholds++];
        }
        h->name = sim_xstrdup(name);
        h->text = sim_xstrdup(text);
        h->ref = ref;
        h->val = cv;
        h->col = col_index(&G.in_cols, &G.nin_cols, name);
        state_changed();
    }
    sim_mutex_unlock(G.mu);
    send_resp(s, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, NULL);
    sj_free(v);
}

static void add_dropped(const char *name, const char *kind, const char *msg)
{
    if (G.dropped.len) sim_buf_addc(&G.dropped, ',');
    sim_buf_adds(&G.dropped, "{\"name\":");
    sim_buf_json_str(&G.dropped, name);
    sim_buf_printf(&G.dropped, ",\"kind\":\"%s\",\"message\":", kind);
    sim_buf_json_str(&G.dropped, msg);
    sim_buf_addc(&G.dropped, '}');
}

/* 引数・戻り値を参照している監視・固定は関数を変えたら解決し直す（リセット後も同じ手順を通す）。
 * 解決し直せないもの（新しい関数にない @引数、前の関数の戻り値など）は外して dropped に載せる。
 * 古い参照を残すと、前の関数の引数番号のまま新しい関数の別の引数を読み書きしてしまう */
static void reresolve_locked(void)
{
    int i, n;
    char err[1024];
    for (i = n = 0; i < G.nwatch; i++) {
        sim_ref *r = sim_ref_parse(G.c, G.watch[i].name, err, sizeof err);
        if (!r) {
            add_dropped(G.watch[i].name, "watch", err);
            free(G.watch[i].name);
            sim_ref_free(G.watch[i].ref);
            continue;
        }
        sim_ref_free(G.watch[i].ref);
        G.watch[i].ref = r;
        G.watch[n++] = G.watch[i];
    }
    G.nwatch = n;
    for (i = n = 0; i < G.nholds; i++) {
        sim_ref *r = sim_ref_parse(G.c, G.holds[i].name, err, sizeof err);
        if (r && !sim_ref_writable(r)) {
            snprintf(err, sizeof err, "'%s' には書き込めません", G.holds[i].name);
            sim_ref_free(r);
            r = NULL;
        }
        if (!r) {
            add_dropped(G.holds[i].name, "hold", err);
            free_hold(&G.holds[i]);
            continue;
        }
        sim_ref_free(G.holds[i].ref);
        G.holds[i].ref = r;
        G.holds[n++] = G.holds[i];
    }
    G.nholds = n;
    G.watch_ver++;
    state_changed();
    values_changed();
}

/* 関数を探して対象にできるか確かめる（状態は変えない）。できなければ NULL と err */
static const sim_func *check_func_locked(const char *name, char *err, size_t errlen)
{
    const sim_func *f;
    if (strlen(name) >= sizeof G.func) {
        snprintf(err, errlen, "関数名 '%.64s...' が長すぎます（%u 文字まで）", name, (unsigned)sizeof G.func - 1);
        return NULL;
    }
    f = sim_ctx_find_func(G.c, name, err, errlen);
    if (!f || sim_ctx_check_func(f, err, errlen)) return NULL;
    return f;
}

/* 検証済みの関数に差し替える */
static void commit_func_locked(const char *name, const sim_func *f)
{
    char err[1024];
    if (sim_ctx_set_func(G.c, f, err, sizeof err)) { set_error(err); return; }  /* 検証済みなので起きない */
    snprintf(G.func, sizeof G.func, "%s", name);
    reresolve_locked();
}

/* 失敗したら以前の関数・引数・表示名をそのまま残す */
static int set_func_locked(const char *name, char *err, size_t errlen)
{
    const sim_func *f = check_func_locked(name, err, errlen);
    if (!f) return -1;
    commit_func_locked(name, f);
    return 0;
}

/* 未消化の単発書き込みを捨てる（リセット前の入力が次のステップに混ざらないように） */
static void discard_pending_locked(void)
{
    int i;
    for (i = 0; i < G.npend; i++) free_pend(&G.pend[i]);
    G.npend = 0;
}

static void reset_locked(void)
{
    char err[1024];
    discard_pending_locked();
    if (G.b->reset(G.b, err, sizeof err)) set_error(err);
    sim_ctx_clear_args(G.c);
    G.step = 0;
    G.overrun = 0;
    G.budget = 0;
    G.play_pos = 0;
    rec_clear();
    reresolve_locked();
    state_changed();
    values_changed();
}

static void free_play_locked(void);

static void api_control(sim_sock s, req_t *r)
{
    sj_value *v = parse_body(s, r);
    const char *cmd, *fn;
    char err[1024];
    sim_buf b;
    sj_value *x;
    const sim_func *nf = NULL;
    if (!v) return;
    cmd = sj_get_str(v, "cmd", "");
    /* 先に全部確かめる。400 を返すときは何も変えない */
    if (cmd[0] && strcmp(cmd, "run") != 0 && strcmp(cmd, "pause") != 0 && strcmp(cmd, "step") != 0 &&
        strcmp(cmd, "reset") != 0) {
        snprintf(err, sizeof err, "cmd '%s' は分かりません（run / pause / step / reset）", cmd);
        send_error(s, 400, err);
        sj_free(v);
        return;
    }
    sim_mutex_lock(G.mu);
    fn = sj_get_str(v, "func", NULL);
    if (fn && strcmp(fn, G.func) != 0) {
        nf = check_func_locked(fn, err, sizeof err);
        if (!nf) {
            sim_mutex_unlock(G.mu);
            send_error(s, 400, err);
            sj_free(v);
            return;
        }
    }
    G.error[0] = 0;
    G.dropped.len = 0;
    if ((x = sj_get(v, "speed")) && x->type == SJ_NUM) {
        double sp = x->num;
        if (sp <= 0) sp = 0;
        else if (sp < 0.1) sp = 0.1;
        else if (sp > 100) sp = 100;
        G.speed = sp;
    }
    if ((x = sj_get(v, "periodMs")) && x->type == SJ_NUM && x->num > 0) {
        G.period_ms = x->num < 0.01 ? 0.01 : (x->num > 60000 ? 60000 : x->num);
    }
    if (nf) {
        /* 対象関数が変わったらターゲットをリセットして記録を区切る（前の関数の記録は消す）。
         * sim run は mod ブロックの初めにリセットするので、保存した CSV を再実行すると同じ結果になる */
        commit_func_locked(fn, nf);
        /* 読み込んだ CSV の入力列は前の関数で解決してあり、その mod 行も前の関数なので再生をやめる */
        free_play_locked();
        reset_locked();
    }
    if (strcmp(cmd, "run") == 0) G.running = 1;
    else if (strcmp(cmd, "pause") == 0) { G.running = 0; G.budget = 0; }
    else if (strcmp(cmd, "step") == 0) {
        double n = sj_get_num(v, "steps", 1);
        G.running = 0;
        G.budget += n >= 1 ? (uint64_t)n : 1;
    } else if (strcmp(cmd, "reset") == 0) {
        G.running = 0;
        reset_locked();
    }
    state_changed();
    /* step はその場で終わるまで待って結果を返す（最大 5 秒） */
    if (strcmp(cmd, "step") == 0) {
        uint64_t t0 = sim_now_ns();
        while (G.budget > 0 && sim_now_ns() - t0 < 5000000000ULL) sim_cond_wait(G.cv, G.mu, 20);
    }
    sim_buf_init(&b);
    json_state(&b);
    sim_mutex_unlock(G.mu);
    send_json(s, 200, &b);
    sim_buf_free(&b);
    sj_free(v);
}

static void free_play_locked(void)
{
    int i;
    for (i = 0; i < G.play_cols; i++) sim_ref_free(G.play_refs[i]);
    free(G.play_refs);
    free(G.play_col_ids);
    G.play_refs = NULL;
    G.play_col_ids = NULL;
    G.play_cols = 0;
    sim_csv_table_free(&G.play);
    G.play_active = 0;
}

static void api_load_csv(sim_sock s, req_t *r)
{
    sim_csv_table t;
    char err[1024];
    int i;
    sim_buf b;
    const sim_func *nf = NULL, *f;
    sim_ref **refs;
    if (sim_csv_load_table(r->body, r->body_len, &t, err, sizeof err)) { send_error(s, 400, err); return; }
    sim_mutex_lock(G.mu);
    /* 関数と入力列をすべて確かめてから差し替える。400 を返すときは以前の関数・再生・実行状態をそのまま残す */
    if (t.func[0] && strcmp(t.func, G.func) != 0) {
        nf = check_func_locked(t.func, err, sizeof err);
        if (!nf) {
            sim_mutex_unlock(G.mu);
            sim_csv_table_free(&t);
            send_error(s, 400, err);
            return;
        }
    }
    f = nf ? nf : sim_ctx_func(G.c);
    refs = sim_xcalloc((size_t)t.nin + 1, sizeof *refs);
    for (i = 0; i < t.nin; i++) {
        refs[i] = sim_ref_parse_for(G.c, f, t.names[i], err, sizeof err);
        if (!refs[i] || !sim_ref_writable(refs[i])) {
            char m[1200];
            int k;
            if (refs[i]) snprintf(err, sizeof err, "'%s' には書き込めません", t.names[i]);
            snprintf(m, sizeof m, "入力 %d 列目: %s", i + 2, err);
            for (k = 0; k <= i; k++) sim_ref_free(refs[k]);
            free(refs);
            sim_csv_table_free(&t);
            sim_mutex_unlock(G.mu);
            send_error(s, 400, m);
            return;
        }
    }
    G.running = 0;
    G.budget = 0;
    G.dropped.len = 0;
    if (nf) commit_func_locked(t.func, nf);
    free_play_locked();
    G.play = t;
    G.play_cols = t.nin;
    G.play_refs = refs;
    G.play_col_ids = sim_xcalloc((size_t)t.nin + 1, sizeof *G.play_col_ids);
    for (i = 0; i < t.nin; i++) G.play_col_ids[i] = col_index(&G.in_cols, &G.nin_cols, t.names[i]);
    set_watch_locked((const char *const *)t.names, t.nin + t.nout, 1, NULL);
    reset_locked();
    G.play_active = 1;
    G.play_pos = 0;
    G.running = 1;
    state_changed();
    sim_buf_init(&b);
    sim_buf_printf(&b, "{\"rows\":%d,\"names\":[", t.nrows);
    for (i = 0; i < t.nin + t.nout; i++) {
        if (i) sim_buf_addc(&b, ',');
        sim_buf_json_str(&b, t.names[i]);
    }
    sim_buf_adds(&b, "]}");
    sim_mutex_unlock(G.mu);
    send_json(s, 200, &b);
    sim_buf_free(&b);
}

/* CSV のセル。',' '"' や前後の空白を含むなら "..." で囲み、'"' は二重にする（sim run の読み込みと対） */
static void csv_cell(sim_buf *b, const char *v)
{
    size_t n = strlen(v);
    int q = n > 0 && (v[0] == ' ' || v[0] == '\t' || v[n - 1] == ' ' || v[n - 1] == '\t' || v[0] == '"');
    const char *p;
    if (!q && (strchr(v, ',') || strchr(v, '"'))) q = 1;
    if (!q) { sim_buf_adds(b, v); return; }
    sim_buf_addc(b, '"');
    for (p = v; *p; p++) {
        if (*p == '"') sim_buf_addc(b, '"');
        sim_buf_addc(b, *p);
    }
    sim_buf_addc(b, '"');
}

static void api_export(sim_sock s)
{
    sim_buf b;
    uint64_t i, start;
    int k;
    int *in_used, *out_used, nin = 0, nout = 0;
    const char **cells = NULL;
    size_t ncells_cap = 0;
    sim_buf_init(&b);
    sim_mutex_lock(G.mu);
    start = G.total > G.cap ? G.total - G.cap : 0;
    in_used = sim_xcalloc((size_t)G.nin_cols + 1, sizeof *in_used);
    out_used = sim_xcalloc((size_t)G.nout_cols + 1, sizeof *out_used);
    for (i = start; i < G.total; i++) {
        size_t need = (size_t)(G.ring[i % G.cap].nout + G.ring[i % G.cap].nin) + 1;
        if (need > ncells_cap) ncells_cap = need;
    }
    cells = sim_xcalloc(ncells_cap + 1, sizeof *cells);
    /* 記録に値がある列だけを出す */
    for (i = start; i < G.total; i++) {
        rec_row *r = &G.ring[i % G.cap];
        rec_cells(r, cells);
        for (k = 0; k < r->nout; k++) if (cells[k][0]) out_used[k] = 1;
        for (k = 0; k < r->nin; k++) if (cells[r->nout + k][0]) in_used[k] = 1;
    }
    for (k = 0; k < G.nin_cols; k++) nin += in_used[k];
    for (k = 0; k < G.nout_cols; k++) nout += out_used[k];
    /* 対象関数を変えるとリセットして記録を消すので、記録はすべて今の関数のもの */
    sim_buf_printf(&b, "mod,%s,GUI記録,%d,%d\r\n#COMMENT", G.func, nin, nout);
    for (k = 0; k < G.nin_cols; k++) if (in_used[k]) sim_buf_printf(&b, ",%s", G.in_cols[k]);
    for (k = 0; k < G.nout_cols; k++) if (out_used[k]) sim_buf_printf(&b, ",%s", G.out_cols[k]);
    sim_buf_adds(&b, "\r\n");
    for (i = start; i < G.total; i++) {
        rec_row *r = &G.ring[i % G.cap];
        rec_cells(r, cells);
        for (k = 0; k < G.nin_cols; k++) {
            if (!in_used[k]) continue;
            sim_buf_addc(&b, ',');
            if (k < r->nin) csv_cell(&b, cells[r->nout + k]);
        }
        for (k = 0; k < G.nout_cols; k++) {
            if (!out_used[k]) continue;
            sim_buf_addc(&b, ',');
            if (k < r->nout) csv_cell(&b, cells[k]);
        }
        sim_buf_adds(&b, "\r\n");
    }
    free(cells);
    sim_mutex_unlock(G.mu);
    free(in_used);
    free(out_used);
    send_resp(s, 200, "text/csv; charset=utf-8", b.p ? b.p : "", b.len,
              "Content-Disposition: attachment; filename=\"sim_record.csv\"\r\n");
    sim_buf_free(&b);
}

static void api_stream(sim_sock s)
{
    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
                      "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\n";
    uint64_t my_watch = (uint64_t)-1, my_state = (uint64_t)-1, my_values = (uint64_t)-1, my_epoch, cursor, last_state_sent = 0;
    if (sim_send_all(s, hdr, strlen(hdr))) return;
    sim_mutex_lock(G.mu);
    my_epoch = G.epoch;
    cursor = G.total > SSE_MAX_ROWS ? G.total - SSE_MAX_ROWS : 0;
    if (G.total > G.cap && cursor < G.total - G.cap) cursor = G.total - G.cap;
    sim_mutex_unlock(G.mu);
    for (;;) {
        sim_buf b;
        uint64_t now = sim_now_ns();
        sim_buf_init(&b);
        sim_mutex_lock(G.mu);
        if (G.epoch != my_epoch) { my_epoch = G.epoch; cursor = 0; my_watch = (uint64_t)-1; }
        if (G.watch_ver != my_watch) {
            my_watch = G.watch_ver;
            sim_buf_adds(&b, "event: hello\ndata: ");
            json_hello(&b);
            sim_buf_adds(&b, "\n\n");
        }
        if (G.total > cursor) {
            uint64_t i, first = cursor;
            int k;
            if (G.total - first > SSE_MAX_ROWS) first = G.total - SSE_MAX_ROWS;
            if (G.total > G.cap && first < G.total - G.cap) first = G.total - G.cap;
            sim_buf_printf(&b, "event: samples\ndata: {\"step0\":%llu,\"names\":",
                           (unsigned long long)G.ring[first % G.cap].step);
            json_watch_names(&b);
            sim_buf_adds(&b, ",\"rows\":[");
            for (i = first; i < G.total; i++) {
                rec_row *r = &G.ring[i % G.cap];
                if (i > first) sim_buf_addc(&b, ',');
                sim_buf_addc(&b, '[');
                for (k = 0; k < G.nwatch; k++) {
                    int col = G.watch[k].col;
                    if (k) sim_buf_addc(&b, ',');
                    if (col < r->nout) sim_buf_json_num(&b, r->v[col]);
                    else sim_buf_adds(&b, "null");
                }
                sim_buf_addc(&b, ']');
            }
            sim_buf_adds(&b, "]}\n\n");
            cursor = G.total;
        }
        if (G.values_ver != my_values) {
            my_values = G.values_ver;
            sim_buf_adds(&b, "event: values\ndata: ");
            json_values(&b);
            sim_buf_adds(&b, "\n\n");
        }
        if (G.state_ver != my_state || now - last_state_sent > 1000000000ULL) {
            my_state = G.state_ver;
            last_state_sent = now;
            sim_buf_adds(&b, "event: state\ndata: ");
            json_state(&b);
            sim_buf_adds(&b, "\n\n");
        }
        sim_mutex_unlock(G.mu);
        if (b.len && sim_send_all(s, b.p, b.len)) { sim_buf_free(&b); return; }
        sim_buf_free(&b);
        sim_sleep_ms(SSE_INTERVAL_MS);
    }
}

static const char PLACEHOLDER[] =
    "<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\"><title>sim GUI 未ビルド</title>"
    "<style>body{font-family:sans-serif;max-width:40em;margin:3em auto;padding:0 1em;line-height:1.6}"
    "code{background:#eee;padding:0 .3em}</style></head><body>"
    "<h1>GUI が未ビルドです</h1>"
    "<p>実行ファイルの隣に <code>sim_gui.html</code> がありません。"
    "<code>gui/dist/sim_gui.html</code> を用意してから、もう一度ビルドしてください。</p>"
    "<p>サーバは動いています。API（<code>/api/info</code>、<code>/api/stream</code> など）は使えます。</p>"
    "</body></html>";

static void serve_index(sim_sock s)
{
    size_t n;
    char *html = sim_read_file(G.html_path, &n);
    if (html) {
        send_resp(s, 200, "text/html; charset=utf-8", html, n, NULL);
        free(html);
    } else {
        send_resp(s, 200, "text/html; charset=utf-8", PLACEHOLDER, sizeof PLACEHOLDER - 1, NULL);
    }
}

static void handle_conn(void *arg)
{
    sim_sock s = (sim_sock)(intptr_t)arg;
    req_t r;
    int rc;
    sim_sock_set_timeout(s, 10000);
    rc = read_request(s, &r);
    if (rc == -2) { send_error(s, 413, "本文が大きすぎます"); sim_close_sock(s); return; }
    if (rc) { sim_close_sock(s); return; }
    if (strcmp(r.method, "OPTIONS") == 0) send_resp(s, 204, "text/plain", "", 0, NULL);
    else if (strcmp(r.method, "GET") == 0 && (strcmp(r.path, "/") == 0 || strcmp(r.path, "/index.html") == 0 || strcmp(r.path, "/sim_gui.html") == 0)) serve_index(s);
    else if (strcmp(r.method, "GET") == 0 && strcmp(r.path, "/api/info") == 0) api_info(s);
    else if (strcmp(r.method, "GET") == 0 && strcmp(r.path, "/api/symbols") == 0) api_symbols(s, &r);
    else if (strcmp(r.method, "GET") == 0 && strcmp(r.path, "/api/stream") == 0) api_stream(s);
    else if (strcmp(r.method, "GET") == 0 && strcmp(r.path, "/api/export.csv") == 0) api_export(s);
    else if (strcmp(r.method, "POST") == 0 && strcmp(r.path, "/api/control") == 0) api_control(s, &r);
    else if (strcmp(r.method, "POST") == 0 && strcmp(r.path, "/api/watch") == 0) api_watch(s, &r);
    else if (strcmp(r.method, "POST") == 0 && strcmp(r.path, "/api/write") == 0) api_write(s, &r);
    else if (strcmp(r.method, "POST") == 0 && strcmp(r.path, "/api/load-csv") == 0) api_load_csv(s, &r);
    else if (strncmp(r.path, "/api/", 5) == 0) {
        char m[2200];
        snprintf(m, sizeof m, "%s %s はありません", r.method, r.path);
        send_error(s, 404, m);
    } else {
        send_resp(s, 404, "text/plain; charset=utf-8", "not found", 9, NULL);
    }
    free(r.body);
    sim_close_sock(s);
}

int sim_gui_run(sim_ctx *c, const sim_gui_opts *o)
{
    char err[1024], url[128];
    sim_sock ls;
    memset(&G, 0, sizeof G);
    G.c = c;
    G.b = sim_ctx_backend(c);
    G.mu = sim_mutex_new();
    G.cv = sim_cond_new();
    G.period_ms = o->period_ms > 0 ? o->period_ms : 5;
    G.speed = o->speed >= 0 ? o->speed : 1;
    G.cap = o->record_max > 0 ? (size_t)o->record_max : 200000;
    G.ring = sim_xcalloc(G.cap, sizeof *G.ring);
    G.port = o->port;
    {
        char *p = sim_path_join(o->html_dir ? o->html_dir : sim_exe_dir(), "sim_gui.html");
        snprintf(G.html_path, sizeof G.html_path, "%s", p);
        free(p);
    }
    {
        const char *fn = o->func ? o->func : G.b->default_func;
        if (fn && fn[0] && set_func_locked(fn, err, sizeof err)) {
            fprintf(stderr, "sim gui: 警告: %s\n", err);
            if (o->func) return 2;
        }
    }
    if (sim_net_init()) { fprintf(stderr, "sim gui: ネットワークの初期化に失敗しました\n"); return 2; }
    ls = sim_listen_local(o->port, err, sizeof err);
    if (ls == SIM_SOCK_INVALID) { fprintf(stderr, "sim gui: %s\n", err); return 2; }
    if (sim_thread_start(sim_loop, NULL)) { fprintf(stderr, "sim gui: スレッドを作れません\n"); return 2; }
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", o->port);
    fprintf(stdout, "sim gui: %s で待ち受けています（周期 %g ms、関数 %s）。止めるには Ctrl+C\n", url, G.period_ms,
            G.func[0] ? G.func : "未設定");
    {
        FILE *probe = fopen(G.html_path, "rb");
        if (probe) fclose(probe);
        else fprintf(stdout, "sim gui: %s がないので「GUI が未ビルド」のページを返します\n", G.html_path);
    }
    fflush(stdout);
    if (o->open_browser) sim_open_browser(url);
    for (;;) {
        sim_sock cs = sim_accept(ls);
        if (cs == SIM_SOCK_INVALID) { sim_sleep_ms(10); continue; }
        if (sim_thread_start(handle_conn, (void *)(intptr_t)cs)) sim_close_sock(cs);
    }
}
