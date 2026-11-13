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
    /* a theme passed by the caller (mmd -t, mmdc -t) is the theme, as if the source named it: with
     * `look: classic` it selects the classic look in the mermaid 12 diagrams that default to neo */
    static const char *const look[][2] = {
        {"---\nconfig:\n  look: classic\n---\nusecase-beta\nactor A\nGo(Go)\nA --> Go\n",
         "---\nconfig:\n  look: classic\n  theme: dark\n---\nusecase-beta\nactor A\nGo(Go)\nA --> Go\n"},
        {"---\nconfig:\n  look: classic\n---\nagentflow-beta\n  A[start] --> B[done]\n",
         "---\nconfig:\n  look: classic\n  theme: dark\n---\nagentflow-beta\n  A[start] --> B[done]\n"},
    };
    for (size_t i = 0; i < sizeof look / sizeof *look; i++) {
        mmd_buffer api = {}, named = {};
        if (mmd_render_svg(strlen(look[i][0]), look[i][0], &(mmd_options){.theme = MMD_THEME_DARK}, &api, &e) ||
            mmd_render_svg(strlen(look[i][1]), look[i][1], nullptr, &named, &e)) { fprintf(stderr, "%s\n", e.message); return 1; }
        if (api.len != named.len || memcmp(api.data, named.data, api.len)) { fprintf(stderr, "API theme %zu differs\n", i); return 1; }
        mmd_buffer_free(&api);
        mmd_buffer_free(&named);
    }
    puts("shared: ok");
    return 0;
}
