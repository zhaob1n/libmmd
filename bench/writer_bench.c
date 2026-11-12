/* mmd_writer cost: a 200-point bar + line xychart built with the writer and rendered, against rendering
 * the same text handed over ready-made, and against building it by hand with snprintf("%.17g").
 * Prints min µs over many runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mmd.h"

constexpr size_t N = 200;
constexpr int RUNS = 2000;

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void build(mmd_writer *w, const double bar[static N], const double line[static N]) {
    mmd_writer_reset(w);
    mmd_xy_begin(w, "Latency per request size", false);
    mmd_xy_x_range(w, "request", 1, N);
    mmd_xy_y_range(w, "µs", 0, 0);
    mmd_xy_bar(w, "p50", N, bar);
    mmd_xy_line(w, "p99", N, line);
}

/* what a hand-written helper does: round-trip safe, but not shortest, and no escaping */
static size_t by_hand(size_t cap, char out[static cap], const double bar[static N], const double line[static N]) {
    size_t k = (size_t)snprintf(out, cap, "xychart\n    title \"Latency per request size\"\n"
                                          "    x-axis \"request\" 1 --> %zu\n    y-axis \"µs\"\n    bar \"p50\" [", N);
    for (size_t i = 0; i < N; i++) k += (size_t)snprintf(out + k, cap - k, i ? ", %.17g" : "%.17g", bar[i]);
    k += (size_t)snprintf(out + k, cap - k, "]\n    line \"p99\" [");
    for (size_t i = 0; i < N; i++) k += (size_t)snprintf(out + k, cap - k, i ? ", %.17g" : "%.17g", line[i]);
    k += (size_t)snprintf(out + k, cap - k, "]\n");
    return k;
}

int main(void) {
    static double bar[N], line[N];
    srand(1);
    for (size_t i = 0; i < N; i++) {
        bar[i] = 40 + (double)(rand() % 10000) / 1000; /* bench-like: three decimals */
        line[i] = bar[i] * 1.5 + (double)(rand() % 1000) / 100;
    }
    mmd_writer *w = mmd_writer_new();
    build(w, bar, line);
    const char *src;
    size_t n;
    if (mmd_writer_text(w, &src, &n, nullptr)) return 1;
    char *copy = malloc(n);
    if (!copy) return 1;
    memcpy(copy, src, n);

    static char hand[1 << 16];
    double t_write = 1e30, t_render = 1e30, t_both = 1e30, t_hand = 1e30;
    mmd_context *ctx = mmd_context_new();
    for (int r = 0; r < RUNS; r++) {
        const char *svg;
        size_t len;
        double t0 = now_us();
        build(w, bar, line);
        double t1 = now_us();
        if (mmd_context_render_svg(ctx, n, copy, nullptr, &svg, &len, nullptr)) return 1;
        double t2 = now_us();
        build(w, bar, line);
        if (mmd_writer_text(w, &src, &n, nullptr) || mmd_context_render_svg(ctx, n, src, nullptr, &svg, &len, nullptr))
            return 1;
        double t3 = now_us();
        (void)by_hand(sizeof hand, hand, bar, line);
        double t4 = now_us();
        if (t4 - t3 < t_hand) t_hand = t4 - t3;
        if (t1 - t0 < t_write) t_write = t1 - t0;
        if (t2 - t1 < t_render) t_render = t2 - t1;
        if (t3 - t2 < t_both) t_both = t3 - t2;
    }
    printf("xychart %zu points x 2 series, %zu bytes of text (min of %d)\n", N, n, RUNS);
    printf("  write only        %7.2f us\n", t_write);
    printf("  render text       %7.2f us\n", t_render);
    printf("  write + render    %7.2f us  (+%.0f%%)\n", t_both, 100 * (t_both / t_render - 1));
    printf("  snprintf %%.17g    %7.2f us  (hand-written text, for comparison)\n", t_hand);
    mmd_context_free(ctx);
    mmd_writer_free(w);
    free(copy);
    return 0;
}
