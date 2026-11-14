/* mtheme against mermaid's own themes: tests/mtheme_cases.tsv (tools/mtheme_cases.mjs) holds blocks of
 * "= <theme> <n>", n override lines "key\tvalue", then the variables getThemeVariables yields that
 * differ from the theme's first block ("key\tvalue", "-key" for one that disappears), or "!" where
 * it throws. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtheme.h"

static const char *const NAMES[] = {[MT_DEFAULT] = "default", [MT_BASE] = "base", [MT_DARK] = "dark",
                                    [MT_FOREST] = "forest", [MT_NEUTRAL] = "neutral"};
enum { MAXL = 4096 };
static char *lines[1 << 17];
static size_t nlines;

typedef struct Ref { MThemeId id; size_t n; char **k, **v; } Ref;

static Str str(const char *s) { return (Str){.p = s, .n = strlen(s)}; }

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/mtheme_cases.tsv";
    FILE *in = fopen(path, "r");
    if (!in) return perror(path), 1;
    char buf[MAXL];
    while (nlines < countof(lines) && fgets(buf, sizeof buf, in)) {
        buf[strcspn(buf, "\n")] = 0;
        lines[nlines++] = strdup(buf);
    }
    fclose(in);
    Ref ref[countof(NAMES)] = {};
    unsigned blocks = 0, checks = 0, bad = 0;
    AUTO_ARENA Arena a = {};
    for (size_t i = 0; i < nlines;) {
        char th[16];
        size_t nov;
        if (sscanf(lines[i], "= %15s %zu", th, &nov) != 2) { printf("bad line %zu: %s\n", i + 1, lines[i]); return 1; }
        MThemeId id = 0;
        while (id < countof(NAMES) && strcmp(NAMES[id], th)) id++;
        if (id == countof(NAMES)) { printf("bad theme %s\n", th); return 1; }
        size_t head = i++;
        Str ks[64], vs[64];
        for (size_t j = 0; j < nov; j++, i++) {
            char *tab = strchr(lines[i], '\t');
            ks[j] = (Str){.p = lines[i], .n = (size_t)(tab - lines[i])}, vs[j] = str(tab + 1);
        }
        size_t e = i;
        while (e < nlines && lines[e][0] != '=') e++;
        arena_reset(&a);
        MTheme *t = mtheme_new(&a, id, nov, ks, vs);
        blocks++;
        bool throws = e - i == 1 && !strcmp(lines[i], "!");
        if (throws || !t) {
            checks++;
            if (throws != !t && bad++ < 40) printf("FAIL %s: mermaid %s, mtheme %s\n", lines[head], throws ? "throws" : "does not", t ? "does not" : "fails");
            i = e;
            continue;
        }
        Ref *r = &ref[id];
        bool first = !r->k;
        if (first) { r->n = e - i, r->k = calloc(r->n, sizeof *r->k), r->v = calloc(r->n, sizeof *r->v); }
        /* expected: the reference with this block's differences applied */
        for (size_t j = 0; j < (first ? e - i : r->n); j++) {
            const char *k = first ? lines[i + j] : r->k[j], *want;
            if (first) {
                char *tab = strchr(lines[i + j], '\t');
                r->k[j] = strndup(lines[i + j], (size_t)(tab - lines[i + j])), r->v[j] = tab + 1;
                k = r->k[j];
            }
            want = r->v[j];
            for (size_t m = i; m < e; m++) {
                size_t kn = strlen(k);
                if (lines[m][0] == '-' && !strcmp(lines[m] + 1, k)) want = nullptr;
                else if (!strncmp(lines[m], k, kn) && lines[m][kn] == '\t') want = lines[m] + kn + 1;
            }
            Str got = mtheme_get(t, str(k));
            checks++;
            if (want ? !str_eq(got, str(want)) : got.n > 0)
                if (bad++ < 40) printf("FAIL %s [%s]: %s = \"%.*s\", mermaid \"%s\"\n", lines[head], nov ? ks[0].p : "", k, (int)got.n, got.p, want ? want : "(undefined)");
        }
        for (size_t m = i; m < e && !first; m++) { /* variables the reference lacks */
            if (lines[m][0] == '-') continue;
            char *tab = strchr(lines[m], '\t');
            Str k = {.p = lines[m], .n = (size_t)(tab - lines[m])};
            bool known = false;
            for (size_t j = 0; j < r->n && !known; j++) known = str_eq(str(r->k[j]), k);
            if (known) continue;
            Str got = mtheme_get(t, k);
            checks++;
            if (!str_eq(got, str(tab + 1)) && bad++ < 40) printf("FAIL %s: %.*s = \"%.*s\", mermaid \"%s\"\n", lines[head], (int)k.n, k.p, (int)got.n, got.p, tab + 1);
        }
        i = e;
    }
    for (size_t i = 0; i < countof(ref); i++) {
        for (size_t j = 0; j < ref[i].n && ref[i].k; j++) free(ref[i].k[j]);
        free(ref[i].k), free(ref[i].v);
    }
    for (size_t i = 0; i < nlines; i++) free(lines[i]);
    printf("mtheme: %u override sets, %u values, %u failed\n", blocks, checks, bad);
    return bad != 0;
}
