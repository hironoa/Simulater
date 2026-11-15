/*
 * xlgen.c - step 1 of the translation test: assemble the test program, find
 * the called functions with the call profile, write the C source of the
 * translation (xl_unit.c, xl_main.c) and the memory image (xl_image.bin).
 *
 *   xlgen <outdir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rl78.h"
#include "tasm.h"
#include "xlprog.h"

static void put(void *ctx, const char *s, size_t n) { fwrite(s, 1, n, (FILE *)ctx); }

int main(int argc, char **argv)
{
    uint8_t *img = (uint8_t *)calloc(1, 0x100000);
    char err[256], path[1024];
    const char *dir = argc > 1 ? argv[1] : ".";
    rl78_config cfg;
    rl78_cpu *c;
    const uint8_t *prof;
    uint8_t *entries;
    uint32_t list[256], a;
    size_t n = 0, ninsns = 0;
    rl78_xl_leaders ld;
    FILE *f;

    if (!img || tasm_program(xl_prog, img, NULL, err, sizeof err)) { fprintf(stderr, "xlgen: asm error: %s\n", err); return 1; }
    snprintf(path, sizeof path, "%s/xl_image.bin", dir);
    if (!(f = fopen(path, "wb")) || fwrite(img, 1, XL_ROM, f) != XL_ROM) { fprintf(stderr, "xlgen: cannot write %s\n", path); return 1; }
    fclose(f);

    rl78_default_config(&cfg, RL78_CORE_S3);
    cfg.rom_size = XL_ROM;
    cfg.mirror_start = 0xF2000;
    cfg.mirror_end = 0xFC000;
    cfg.mirror_src = 0x02000;
    c = rl78_create(&cfg);
    rl78_load(c, 0, img, XL_ROM);
    rl78_reset(c);

    /* which functions get called: run interpreted with the call profile on */
    if (rl78_set_call_profile(c, 1)) return 1;
    if (rl78_run(c, 200000) != RL78_STOP_LIMIT) { fprintf(stderr, "xlgen: the program stopped: %s\n", rl78_last_fault(c)->text); return 1; }
    prof = rl78_call_profile(c);
    entries = (uint8_t *)calloc(0x100000 / 8, 1);
    memcpy(entries, prof, 0x100000 / 8);
    entries[XL_START >> 3] |= (uint8_t)(1u << (XL_START & 7u));   /* the reset entry is not the target of a CALL */
    for (a = 0; a < 0x100000 && n < 256; a++)
        if ((entries[a >> 3] >> (a & 7u)) & 1) list[n++] = a;
    printf("xlgen: %u functions:", (unsigned)n);
    { size_t i; for (i = 0; i < n; i++) printf(" %05X", (unsigned)list[i]); }
    printf("\n");
    if (n < 8) { fprintf(stderr, "xlgen: the call profile found too few functions\n"); return 1; }

    rl78_xl_leaders_init(&ld);
    snprintf(path, sizeof path, "%s/xl_unit.c", dir);
    if (!(f = fopen(path, "w"))) return 1;
    if (rl78_xlate_unit(c, list, n, entries, put, f, &ld, &ninsns)) { fprintf(stderr, "xlgen: translation failed\n"); return 1; }
    fclose(f);
    snprintf(path, sizeof path, "%s/xl_main.c", dir);
    if (!(f = fopen(path, "w"))) return 1;
    if (rl78_xlate_main(c, &ld, put, f)) return 1;
    fclose(f);
    printf("xlgen: %u instructions translated\n", (unsigned)ninsns);
    rl78_xl_leaders_free(&ld);
    rl78_destroy(c);
    free(entries);
    free(img);
    return ninsns > 50 ? 0 : 1;
}
