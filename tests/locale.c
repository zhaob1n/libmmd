/* A host that sets a comma-decimal LC_NUMERIC must get the same SVG as one in the C locale, and keep
 * its own locale afterwards. Renders every file named on the command line plus inputs that reach each
 * printf/strtod site, in "C" and then in the first comma locale installed; skips if there is none. */
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmd.h"

static const char *const INLINE[] = {
    "%%{init: {\"pie\": {\"textPosition\": 0.6}}}%%\npie\n  \"a\" : 1\n  \"b\" : 3\n",
    "xychart\n  x-axis [a, b, c]\n  y-axis 0.5 --> 2.75\n  bar [0.75, 1.5, 2.25]\n  line [0.6, 1.25, 2.5]\n",
    "pie showData\n  \"a\" : 1.5\n  \"b\" : 2.25\n",
    "---\nconfig:\n  sankey:\n    showValues: true\n---\nsankey\na,b,1.25\nb,c,0.5\n",
    "treemap-beta\n\"s\"\n    \"a\": 1.25\n    \"b\": 2.5\n",
    "%%{init: {\"treeView\": {\"lineThickness\": 1.5}}}%%\ntreeView-beta\n  src\n    a.c\n",
    "wardley-beta\ntitle T\ncomponent A [0.25, 0.75]\ncomponent B [0.5, 0.35]\nA -> B\n",
};

typedef struct { mmd_status st; size_t n; char *svg; } Out;

static Out render(size_t n, const char src[static n]) {
    mmd_buffer b;
    mmd_error e;
    mmd_status st = mmd_render_svg(n, src, nullptr, &b, &e);
    return (Out){st, st ? 0 : b.len, st ? nullptr : b.data};
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
    static const char *const COMMA[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "nl_NL.UTF-8",
                                        "ru_RU.UTF-8", "es_ES.UTF-8", "it_IT.UTF-8", "pt_BR.UTF-8"};
    const char *loc = nullptr;
    for (size_t i = 0; i < sizeof COMMA / sizeof *COMMA && !loc; i++)
        if (setlocale(LC_ALL, COMMA[i]) && !strcmp(localeconv()->decimal_point, ",")) loc = COMMA[i];
    if (!loc) { puts("locale: skipped (no comma-decimal locale installed)"); return 0; }

    size_t ni = sizeof INLINE / sizeof *INLINE, total = ni + (size_t)(argc - 1), bad = 0, rendered = 0;
    char **src = calloc(total, sizeof *src);
    size_t *len = calloc(total, sizeof *len);
    Out *ref = calloc(total, sizeof *ref);
    if (!src || !len || !ref) return 1;
    for (size_t i = 0; i < ni; i++) src[i] = (char *)INLINE[i], len[i] = strlen(INLINE[i]);
    for (int a = 1; a < argc; a++)
        if (!(src[ni + a - 1] = slurp(argv[a], &len[ni + a - 1]))) { fprintf(stderr, "locale: cannot read %s\n", argv[a]); return 1; }

    setlocale(LC_ALL, "C");
    for (size_t i = 0; i < total; i++) ref[i] = render(len[i], src[i]);
    setlocale(LC_ALL, loc);
    mmd_context *ctx = mmd_context_new();
    if (!ctx) return 1;
    for (size_t i = 0; i < total; i++) {
        Out o = render(len[i], src[i]);
        const char *cs = nullptr;
        size_t cn = 0;
        mmd_error e;
        mmd_status cst = mmd_context_render_svg(ctx, len[i], src[i], nullptr, &cs, &cn, &e);
        bool same = o.st == ref[i].st && o.n == ref[i].n && (!o.n || !memcmp(o.svg, ref[i].svg, o.n)) && cst == o.st &&
                    cn == o.n && (!cn || !memcmp(cs, o.svg, cn));
        if (!same && bad++ < 10) {
            if (i < ni) fprintf(stderr, "locale: inline case %zu differs under %s\n", i, loc);
            else fprintf(stderr, "locale: %s differs under %s\n", argv[i - ni + 1], loc);
        }
        rendered += !ref[i].st;
        free(o.svg);
    }
    mmd_context_free(ctx);
    for (size_t i = 0; i < total; i++) {
        free(ref[i].svg);
        if (i >= ni) free(src[i]);
    }
    free(src), free(len), free(ref);
    char probe[16];
    snprintf(probe, sizeof probe, "%g", 0.5);
    if (strcmp(probe, "0,5")) { fprintf(stderr, "locale: the host's %s was not restored (%s)\n", loc, probe); bad++; }
    printf("locale: %zu inputs (%zu rendered) under %s, %zu differ from C\n", total, rendered, loc, bad);
    return bad != 0;
}
