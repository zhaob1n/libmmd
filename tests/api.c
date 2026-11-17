/* The API as a program uses it: one-shot and context renders agree, a context's kept theme, and
 * the theme a caller passes. */
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
    /* a context keeps the theme it derived last: the same theme and themeVariables give it back,
     * anything else must not, and nothing kept may point into an earlier source (one buffer, rewritten) */
    static const char *const themed[] = {
        "---\nconfig:\n  theme: base\n  themeVariables:\n    primaryColor: \"#ffcc00\"\n---\nflowchart LR\n A-->B\n",
        "---\nconfig:\n  theme: base\n  themeVariables:\n    primaryColor: \"#00ccff\"\n---\nflowchart LR\n A-->B\n",
        "---\nconfig:\n  theme: forest\n  themeVariables:\n    primaryColor: \"#ffcc00\"\n---\nflowchart LR\n A-->B\n",
        "---\nconfig:\n  theme: base\n  themeVariables:\n    primaryColor: \"#ffcc00\"\n    lineColor: \"#ff0000\"\n---\nflowchart LR\n A-->B\n",
    };
    static constexpr unsigned steps[] = {0, 0, 1, 0, 2, 0, 3, 0};
    c = mmd_context_new();
    if (!c) return 1;
    for (size_t i = 0; i < sizeof steps / sizeof *steps; i++) {
        const char *s = themed[steps[i]];
        char text[256];
        size_t len = strlen(s);
        memcpy(text, s, len);
        mmd_buffer one;
        if (mmd_render_svg(len, s, nullptr, &one, &e) || mmd_context_render_svg(c, len, text, nullptr, &svg, &n, &e)) {
            fprintf(stderr, "%s\n", e.message);
            return 1;
        }
        if (n != one.len || memcmp(svg, one.data, n)) { fprintf(stderr, "context theme, step %zu differs\n", i); return 1; }
        mmd_buffer_free(&one);
    }
    mmd_context_free(c);
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
    puts("api: ok");
    return 0;
}
