/* sim_cc.c - コンパイラ（clang）を並列に起動する部分と、キャッシュのフォルダの後始末（sim_cc.h） */
#include "sim_cc.h"

#include <stdlib.h>
#include <string.h>

#include "sim_util.h"

uint64_t sim_cc_fnv(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

int sim_cc_run_wait(const char *const *argv, const char *log)
{
    sim_proc *p = sim_proc_spawn(argv, log);
    return p ? sim_proc_wait(p) : -1;
}

int sim_cc_works(const char *cc)
{
    const char *argv[3];
    argv[0] = cc;
    argv[1] = "--version";
    argv[2] = NULL;
    return sim_cc_run_wait(argv, NULL) == 0;
}

static void progress(sim_cc_job **slots, size_t nslots, size_t done, size_t total,
                     uint64_t start, uint64_t now, const char *label, sim_cc_say_fn say, void *ctx)
{
    sim_cc_job *top[3] = {NULL, NULL, NULL};
    size_t i, j, running = 0;
    sim_buf line;
    /* 長く動いているものを先に見せ、止まって見える原因を探しやすくする */
    for (i = 0; i < nslots; i++) {
        sim_cc_job *job = slots[i];
        if (!job) continue;
        running++;
        for (j = 0; j < 3; j++) {
            if (!top[j] || job->started < top[j]->started) {
                size_t k;
                for (k = 2; k > j; k--) top[k] = top[k - 1];
                top[j] = job;
                break;
            }
        }
    }
    sim_buf_init(&line);
    sim_buf_printf(&line, "%s: コンパイル中 %u/%u ファイル（経過 %.0f 秒。実行中: ", label,
                   (unsigned)done, (unsigned)total, (double)((now - start) / 1000000000ULL));
    for (i = 0; i < 3 && top[i]; i++)
        sim_buf_printf(&line, "%s%s %.0f 秒", i ? ", " : "", top[i]->name,
                       (double)((now - top[i]->started) / 1000000000ULL));
    if (!running) sim_buf_adds(&line, "なし");
    if (running > 3) sim_buf_printf(&line, " ほか %u 個", (unsigned)(running - 3));
    sim_buf_adds(&line, "）\n");
    say(ctx, sim_buf_cstr(&line));
    sim_buf_free(&line);
}

int sim_cc_run_jobs(sim_cc_job *jobs, size_t njobs, int parallel, const char *log,
                    const char *label, sim_cc_say_fn say, void *ctx)
{
    size_t next = 0, done = 0, i;
    size_t nslots = parallel > 0 && (size_t)parallel < njobs ? (size_t)parallel : njobs;
    sim_cc_job **slots;
    uint64_t start = sim_now_ns(), last = start;
    int failed = 0;

    if (!njobs) return 0;
    slots = sim_xcalloc(nslots, sizeof *slots);
    while (done < njobs) {
        int finished = 0;
        uint64_t now;
        for (i = 0; i < nslots; i++) {
            sim_cc_job *job = slots[i];
            int code;
            if (job && sim_proc_poll(job->proc, &code)) {
                job->elapsed = sim_now_ns() - job->started;
                job->proc = NULL;
                job->exit_code = code;
                slots[i] = NULL;
                if (code != 0) failed = 1;
                done++;
                finished = 1;
            }
            while (!slots[i] && next < njobs) {
                job = &jobs[next++];
                job->started = sim_now_ns();
                job->proc = sim_proc_spawn(job->argv, log);
                if (job->proc) {
                    job->compiled = 1;
                    slots[i] = job;
                } else {
                    job->exit_code = -1;
                    failed = 1;
                    done++;
                }
            }
        }
        now = sim_now_ns();
        if (say && now - start > 5000000000ULL &&
            ((finished && now - last >= 5000000000ULL) || now - last >= 10000000000ULL)) {
            progress(slots, nslots, done, njobs, start, now, label, say, ctx);
            last = now;
        }
        if (done < njobs) sim_sleep_ms(15);
    }
    free(slots);
    return failed ? -1 : 0;
}

static int time_cmp(const void *a, const void *b)
{
    const sim_cc_job *ja = *(const sim_cc_job *const *)a, *jb = *(const sim_cc_job *const *)b;
    return ja->elapsed > jb->elapsed ? -1 : ja->elapsed < jb->elapsed;
}

void sim_cc_write_times(const sim_cc_job *jobs, size_t njobs, const char *dir,
                        const char *label, sim_cc_say_fn say, void *ctx)
{
    const sim_cc_job **order = sim_xcalloc(njobs ? njobs : 1, sizeof *order);
    size_t i, count = 0;
    sim_buf times, slow, line;
    char *path;
    for (i = 0; i < njobs; i++) if (jobs[i].compiled) order[count++] = &jobs[i];
    if (count < 2) { free(order); return; }
    qsort(order, count, sizeof *order, time_cmp);
    sim_buf_init(&times);
    sim_buf_init(&slow);
    sim_buf_init(&line);
    for (i = 0; i < count; i++) {
        const sim_cc_job *job = order[i];
        sim_buf_printf(&times, "%s\t%.1f\n", job->name, (double)job->elapsed / 1e9);
        if (i < 5 && job->elapsed >= 30000000000ULL)
            sim_buf_printf(&slow, "%s%s %.0f 秒", i ? ", " : "", job->name,
                           (double)(job->elapsed / 1000000000ULL));
    }
    path = sim_path_join(dir, "cc_times.txt");
    if (sim_write_file(path, times.p, times.len) && say) {
        sim_buf_printf(&line, "%s: %s に書けません\n", label, path);
        say(ctx, sim_buf_cstr(&line));
        line.len = 0;
    }
    if (slow.len && say) {
        sim_buf_printf(&line, "%s: 時間のかかったファイル: %s\n", label, sim_buf_cstr(&slow));
        say(ctx, sim_buf_cstr(&line));
    }
    free(path);
    sim_buf_free(&times);
    sim_buf_free(&slow);
    sim_buf_free(&line);
    free(order);
}

static void names_visit(void *ctx, const char *name, int kind)
{
    sim_buf *names = ctx;
    (void)kind;
    sim_buf_adds(names, name);
    sim_buf_addc(names, '\n');
}

void sim_cc_remove_dir_files(const char *dir)
{
    sim_buf names;
    char *p, *q;
    sim_buf_init(&names);
    sim_dir_each(dir, names_visit, &names);
    for (p = (char *)sim_buf_cstr(&names); p && *p; p = q ? q + 1 : NULL) {
        char *f;
        q = strchr(p, '\n');
        if (q) *q = 0;
        f = sim_path_join(dir, p);
        sim_remove_file(f);
        free(f);
    }
    sim_buf_free(&names);
    sim_remove_dir(dir);
}

void sim_cc_sweep_old(const char *base, const char *prefix, const char *keep)
{
    sim_buf names;
    char *p, *q;
    sim_buf_init(&names);
    sim_dir_each(base, names_visit, &names);
    for (p = (char *)sim_buf_cstr(&names); p && *p; p = q ? q + 1 : NULL) {
        q = strchr(p, '\n');
        if (q) *q = 0;
        if (!strncmp(p, prefix, strlen(prefix)) && strcmp(p, keep)) {
            char *d = sim_path_join(base, p);
            if (sim_path_kind(d) == 2) sim_cc_remove_dir_files(d);
            free(d);
        }
    }
    sim_buf_free(&names);
}
