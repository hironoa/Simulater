/* Cross-platform GUI API contract test. Uses curl and the runtime JSON parser. */
#define _CRT_SECURE_NO_WARNINGS
#include "sim_json.h"
#include "sim_util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

static const char *url, *work;
static int checks;
static char *seen[5000];
static int nseen;

static void require(int ok, const char *name)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s (check %d)\n", name, checks); exit(1); }
}

static sj_value *request(const char *path, const char *query, const char *body)
{
    sim_buf cmd, data;
    char tmp[4096], err[256];
    FILE *f;
    size_t n;
    sj_value *v;
    sim_buf_init(&cmd);
    sim_buf_init(&data);
    if (body) {
        char file[2048];
        snprintf(file, sizeof file, "%s/symbol-request.json", work);
        f = fopen(file, "wb");
        require(f != NULL, "request file");
        fputs(body, f);
        fclose(f);
        sim_buf_printf(&cmd, "curl -s --max-time 5 -X POST --data-binary \"@%s\"", file);
    } else {
        const char *p;
        /* Only expression characters: keep shell quoting safe on both platforms. */
        for (p = query; *p; p++)
            require(isalnum((unsigned char)*p) || strchr("_./@[] ->", *p) != NULL, "query character");
        sim_buf_printf(&cmd, "curl -s --max-time 5 -G --data-urlencode \"q=%s\" --data-urlencode \"limit=5000\"", query);
    }
    sim_buf_printf(&cmd, " -w \"\\n%%{http_code}\" \"%s%s\"", url, path);
    f = popen(cmd.p, "r");
    require(f != NULL, "curl start");
    while ((n = fread(tmp, 1, sizeof tmp, f)) != 0) sim_buf_add(&data, tmp, n);
    require(pclose(f) == 0, "curl exit");
    require(data.len >= 4 && strcmp(data.p + data.len - 4, "\n200") == 0, "HTTP 200");
    data.len -= 4;
    v = sj_parse(data.p, data.len, err, sizeof err);
    require(v != NULL, "JSON response");
    sim_buf_free(&cmd);
    sim_buf_free(&data);
    return v;
}

static sj_value *exact(sj_value *v, const char *name)
{
    sj_value *e;
    require(v->type == SJ_ARR, "symbols array");
    for (e = v->child; e; e = e->next)
        if (strcmp(sj_get_str(e, "name", ""), name) == 0) return e;
    require(0, name);
    return NULL;
}

static int same(sj_value *a, sj_value *b)
{
    sj_value *x, *y;
    if (!a || !b || a->type != b->type) return 0;
    if (a->type == SJ_STR) return strcmp(a->str, b->str) == 0;
    if (a->type == SJ_NUM || a->type == SJ_BOOL) return a->num == b->num;
    if (a->type == SJ_ARR) {
        for (x = a->child, y = b->child; x && y; x = x->next, y = y->next)
            if (!same(x, y)) return 0;
        return !x && !y;
    }
    return 0;
}

static void visit(sj_value *entry, int depth)
{
    const char *name = sj_get_str(entry, "name", "");
    const char *fields[] = {"type", "size", "kind", "writable", "members"};
    sj_value *hits, *e, *m, *members;
    int i;
    sim_buf body;
    for (i = 0; i < nseen; i++) if (strcmp(seen[i], name) == 0) return;
    require(nseen < 5000 && depth <= 12 && name[0], "nonempty bounded traversal");
    seen[nseen++] = sim_xstrdup(name);
    hits = request("/api/symbols", name, NULL);
    e = exact(hits, name);
    for (i = 0; i < 5; i++) require(same(sj_get(e, fields[i]), sj_get(entry, fields[i])), name);
    members = sj_get(e, "members");
    require(members && members->type == SJ_ARR, "members array");
    if (strcmp(sj_get_str(e, "kind", ""), "struct") != 0 && strcmp(sj_get_str(e, "kind", ""), "array") != 0) {
        sim_buf_init(&body);
        sim_buf_adds(&body, "{\"names\":[");
        sim_buf_json_str(&body, name);
        sim_buf_adds(&body, "]}");
        m = request("/api/watch", NULL, body.p);
        require(sj_get(m, "errors") && sj_get(m, "errors")->nchild == 0, name);
        sj_free(m);
        sim_buf_free(&body);
        if (sj_get_bool(e, "writable", 0)) {
            sim_buf_init(&body);
            sim_buf_adds(&body, "{\"name\":");
            sim_buf_json_str(&body, name);
            sim_buf_adds(&body, ",\"value\":0}");
            m = request("/api/write", NULL, body.p);
            require(sj_get_bool(m, "ok", 0), name);
            sj_free(m);
            sim_buf_free(&body);
        }
    }
    for (m = members->child; m; m = m->next) {
        sj_value *child = request("/api/symbols", m->str, NULL);
        visit(exact(child, m->str), depth + 1);
        sj_free(child);
    }
    sj_free(hits);
}

static void expect_symbol(const char *name, const char *type, int size, int writable)
{
    sj_value *hits = request("/api/symbols", name, NULL), *e = exact(hits, name);
    require(strcmp(sj_get_str(e, "type", ""), type) == 0, name);
    require(sj_get_num(e, "size", -1) == size, name);
    require(sj_get_bool(e, "writable", !writable) == writable, name);
    visit(e, 0);
    sj_free(hits);
}

int main(int argc, char **argv)
{
    sj_value *all, *e, *reset;
    int i;
    if (argc != 4) return 2;
    url = argv[1]; work = argv[2];
    expect_symbol("g_cal.lo", strcmp(argv[3], "mock") == 0 ? "uint8" : "uint8_t", 1, 1);
    expect_symbol("g_cal.hi", strcmp(argv[3], "mock") == 0 ? "uint8" : "uint8_t", 1, 1);
    all = request("/api/symbols", ".lo", NULL);
    visit(exact(all, "g_cal.lo"), 0);
    sj_free(all);
    all = request("/api/symbols", ".hi", NULL);
    visit(exact(all, "g_cal.hi"), 0);
    sj_free(all);
    if (strcmp(argv[3], "mock") == 0) {
        expect_symbol("g_cal_const.lo", "uint8", 1, 0);
        expect_symbol("g_cal_array[1].lo", "uint8", 1, 1);
        expect_symbol("gp_cal->lo", "uint8", 1, 1);
        expect_symbol("g_search.bytes[2]", "uint8", 1, 1);
        expect_symbol("g_search.fan_req", "uint8", 1, 1);
        expect_symbol("g_big[257]", "uint8", 1, 1);
        expect_symbol("g_deep[0][0][0][0][0][0][0][0][0][0]", "uint8", 1, 1);
        expect_symbol("g_cal_array [ 1 ] .lo", "uint8", 1, 1);
        all = request("/api/symbols", "", NULL);
        require(all->type == SJ_ARR && all->nchild > 0 && all->nchild < 5000, "all symbols, no truncation");
        for (e = all->child; e; e = e->next) visit(e, 0);
        sj_free(all);
    }
    reset = request("/api/control", NULL, "{\"cmd\":\"reset\"}");
    sj_free(reset);
    require(nseen > 0, "nonzero symbols");
    printf("symbols contract: %d symbols, %d checks, 0 failures\n", nseen, checks);
    for (i = 0; i < nseen; i++) free(seen[i]);
    return 0;
}
