/* バックエンドが修飾型を共有していても、引数の準備と型照会を両立する。 */
#include "sim_expr.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"const refs line %d: %s (%s)\n",__LINE__,#x,err); } } while (0)
int main(void)
{
    sim_open_opts o = {0};
    char err[512] = "";
    sim_backend *b = sim_backend_open(&o, err, sizeof err);
    sim_ctx *c;
    const sim_func *original;
    int mode;
    CHECK(b != NULL);
    if (!b) return 1;
    c = sim_ctx_new(b);
    original = sim_ctx_find_func(c, "eng_sum", err, sizeof err);
    CHECK(original != NULL);
    if (!original) return 1;
    for (mode = 0; mode < 4; mode++) {
        sim_func f = *original;
        sim_type elem = *original->param_types[0]->target;
        sim_type ptr = *original->param_types[0];
        sim_type n = *original->param_types[1];
        const sim_type *params[] = {&ptr, &n};
        const char *names[] = {"@buf", "$@buf", "@buf[0]", "*@buf", "eng_sum@buf[0]", "@n"};
        const char *values[] = {"0xE000", "2", "123", "123|456", "789", "2"};
        unsigned i;
        ptr.is_const = mode & 1; elem.is_const = (mode >> 1) & 1;
        ptr.target = &elem; n.is_const = 1; f.param_types = params;
        CHECK(sim_ctx_set_func(c, &f, err, sizeof err) == 0);
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            sim_ref *r = sim_ref_parse(c, names[i], err, sizeof err);
            sim_ref *symbol = sim_ref_parse_symbol(c, names[i], err, sizeof err);
            sim_cellval v;
            CHECK(r != NULL && symbol != NULL);
            if (!r || !symbol) { sim_ref_free(r); sim_ref_free(symbol); continue; }
            CHECK(sim_ref_writable(r) && sim_ref_writable(symbol));
            CHECK(sim_parse_cell(values[i], strlen(values[i]), &v, err, sizeof err) == 0);
            CHECK(sim_ref_write(c, r, &v, err, sizeof err) == 0);
            sim_cellval_free(&v); sim_ref_free(r); sim_ref_free(symbol);
        }
        CHECK(ptr.is_const == (mode & 1) && elem.is_const == ((mode >> 1) & 1) && n.is_const);
        CHECK(original->param_types[0]->is_const == 0);
    }
    sim_ctx_set_func(c, NULL, err, sizeof err);
    sim_ctx_free(c); b->close(b);
    printf("const refs checks=%u failures=%u\n",checks,failures);
    return !checks || failures;
}
