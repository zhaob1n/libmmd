/* in-process benchmark: bench file.mmd [iters] -> prints min/median us per render */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "cmermaid.h"

static double now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}
static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc < 2) { fputs("usage: bench file.mmd [iters]\n", stderr); return 2; }
    int iters = argc > 2 ? atoi(argv[2]) : 200;
    if (iters < 1) iters = 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }
    char *src = malloc((size_t)sz + 1);
    double *t = malloc(sizeof(double) * (size_t)iters);
    if (!src || !t || fread(src, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(src); free(t); return 1; }
    fclose(f);
    size_t outlen = 0;
    bool use_ctx = argc > 3 && strcmp(argv[3], "ctx") == 0;
    cmm_context *ctx = use_ctx ? cmm_context_new() : nullptr;
    if (use_ctx && !ctx) { free(src); free(t); return 1; }
    for (int i = 0; use_ctx && i < iters; i++) {
        const char *svg;
        cmm_error e;
        double t0 = now_us();
        cmm_status st = cmm_context_render_svg(ctx, (size_t)sz, src, nullptr, &svg, &outlen, &e);
        t[i] = now_us() - t0;
        if (st) { fprintf(stderr, "error: %s\n", e.message); cmm_context_free(ctx); free(src); free(t); return 1; }
    }
    cmm_context_free(ctx);
    for (int i = 0; !use_ctx && i < iters; i++) {
        cmm_buffer b;
        cmm_error e;
        double t0 = now_us();
        cmm_status st = cmm_render_svg((size_t)sz, src, nullptr, &b, &e);
        t[i] = now_us() - t0;
        if (st) { fprintf(stderr, "error: %s\n", e.message); free(src); free(t); return 1; }
        outlen = b.len;
        cmm_buffer_free(&b);
    }
    qsort(t, (size_t)iters, sizeof *t, cmpd);
    printf("%s: bytes_in=%ld svg_bytes=%zu min=%.1fus median=%.1fus\n", argv[1], sz, outlen, t[0], t[iters / 2]);
    free(src);
    free(t);
    return 0;
}
