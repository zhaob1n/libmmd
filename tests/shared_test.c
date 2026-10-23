/* Links against libmmd.so: checks exported API incl. context reuse. */
#include <stdio.h>
#include <string.h>
#include "mmd.h"

int main(void) {
    static constexpr char src[] = "flowchart LR\n A-->B\n B-->C";
    mmd_buffer b;
    mmd_error e;
    if (mmd_render_svg(sizeof src - 1, src, nullptr, &b, &e)) { fprintf(stderr, "%s\n", e.message); return 1; }
    mmd_context *c = mmd_context_new();
    if (!c) return 1;
    for (int i = 0; i < 50; i++) {
        const char *svg;
        size_t n;
        if (mmd_context_render_svg(c, sizeof src - 1, src, nullptr, &svg, &n, &e) || n != b.len ||
            memcmp(svg, b.data, n) != 0) { fputs("context mismatch\n", stderr); return 1; }
    }
    static constexpr char bad[] = "flowchart LR\n A-->";
    const char *svg;
    size_t n;
    if (mmd_context_render_svg(c, sizeof bad - 1, bad, nullptr, &svg, &n, &e) == MMD_OK) puts("note: lenient parse");
    if (mmd_context_render_svg(c, sizeof src - 1, src, nullptr, &svg, &n, &e) || n != b.len) return 1;
    mmd_context_free(c);
    mmd_buffer_free(&b);
    puts("shared: ok");
    return 0;
}
