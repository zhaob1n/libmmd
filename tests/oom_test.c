/* Allocation-failure injection. libmmd's malloc/calloc/realloc are wrapped at link time (-Wl,--wrap,
 * see the Makefile); a library built with -DMMD_FAULT_INJECT (`make oom`) also asks mmd_fault_inject()
 * before every arena allocation. Every input is rendered once to count those calls, then again with
 * one of them failing: a render must report MMD_ERR_NOMEM or, where the allocation had a fallback,
 * succeed with the very same SVG. Under ASan this also shows the failure paths leak nothing and touch
 * nothing freed. Inputs with many allocations are sampled: the first SAMPLE, then SAMPLE spread over
 * the rest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmd.h"

void *__real_malloc(size_t n);
void *__real_calloc(size_t k, size_t n);
void *__real_realloc(void *p, size_t n);

static size_t calls, fail_at; /* fail the fail_at-th call (1-based); 0: never */
static bool hit(void) { return ++calls == fail_at; }
void *__wrap_malloc(size_t n) { return hit() ? nullptr : __real_malloc(n); }
void *__wrap_calloc(size_t k, size_t n) { return hit() ? nullptr : __real_calloc(k, n); }
void *__wrap_realloc(void *p, size_t n) { return hit() ? nullptr : __real_realloc(p, n); }
bool mmd_fault_inject(void) { return hit(); }

enum { SAMPLE = 64 };
/* the next allocation to fail after k, or 0 when done */
static size_t next_fail(size_t k, size_t total) {
    if (k < SAMPLE || total <= 2 * SAMPLE) return k < total ? k + 1 : 0;
    size_t step = (total - SAMPLE + SAMPLE - 1) / SAMPLE;
    return k + step <= total ? k + step : 0;
}

static char *slurp(const char *path, size_t n[static 1]) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    char *p = nullptr;
    size_t cap = 0;
    *n = 0;
    for (size_t k; (p = realloc(p, cap += 1 << 16)) && (k = fread(p + *n, 1, cap - *n, f)) > 0;) *n += k;
    fclose(f);
    return p;
}

int main(int argc, char **argv) {
    size_t files = 0, renders = 0, bad = 0;
    for (int a = 1; a < argc; a++) {
        size_t n;
        char *src = slurp(argv[a], &n);
        if (!src) { fprintf(stderr, "oom: cannot read %s\n", argv[a]); return 1; }
        mmd_buffer ref;
        mmd_error e;
        calls = fail_at = 0;
        mmd_status rst = mmd_render_svg(n, src, nullptr, &ref, &e);
        size_t total = calls, wrong = 0;
        for (fail_at = total ? 1 : 0; fail_at; fail_at = next_fail(fail_at, total)) {
            calls = 0;
            mmd_buffer out;
            mmd_status st = mmd_render_svg(n, src, nullptr, &out, &e);
            bool same = st == rst && (st || (out.len == ref.len && !memcmp(out.data, ref.data, ref.len)));
            if (st != MMD_ERR_NOMEM && !same && !wrong++) /* the first per input is enough to go on */
                fprintf(stderr, "oom: %s: failing allocation %zu of %zu gave '%s' (%s without it)\n", argv[a], fail_at,
                        total, mmd_status_str(st), mmd_status_str(rst));
            if (!st) mmd_buffer_free(&out);
            renders++;
        }
        bad += wrong;
        fail_at = 0;
        if (!rst) mmd_buffer_free(&ref);
        free(src);
        files++;
    }
    printf("oom: %zu inputs, %zu failing renders, %zu wrong\n", files, renders, bad);
    return bad != 0;
}
