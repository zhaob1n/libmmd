/* mmd_writer round trip: random labels full of mermaid syntax go through each escape context into every
 * diagram type; the rendered SVG must show them verbatim, or the writer must have refused them. */
#include <math.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmd_writer.h"

typedef enum Ctx { STR, TEXT, CSV, ID } Ctx;
typedef struct Case {
    const char *name, *pre, *post;
    Ctx ctx;
    bool br; /* the diagram draws line breaks here */
} Case;

static const Case CASES[] = {
    {"flowchart node", "flowchart LR\n  A[", "] --> B\n", STR, true},
    {"flowchart round", "flowchart LR\n  A(", ") --> B\n", STR, true},
    {"flowchart rhombus", "flowchart LR\n  A{", "} --> B\n", STR, true},
    {"flowchart @{label}", "flowchart LR\n  A@{ shape: rect, label: ", " } --> B\n", STR, true},
    {"flowchart edge |l|", "flowchart LR\n  A -->|", "| B\n", STR, true},
    {"flowchart edge -- l -->", "flowchart LR\n  A -- ", " --> B\n", STR, true},
    {"flowchart subgraph", "flowchart LR\n  subgraph s1 [", "]\n    A\n  end\n", STR, true},
    {"swimlane node", "swimlane-beta LR\n  subgraph L\n    A[", "]\n  end\n", STR, true},
    {"swimlane lane", "swimlane-beta LR\n  subgraph l1 [", "]\n    A\n  end\n", STR, true},
    {"sequence message", "sequenceDiagram\n  A->>B: ", "\n", TEXT, true},
    {"sequence note", "sequenceDiagram\n  A->>B: x\n  Note over A: ", "\n", TEXT, true},
    {"sequence alias", "sequenceDiagram\n  participant A as ", "\n  A->>B: x\n", TEXT, true},
    {"sequence loop", "sequenceDiagram\n  loop ", "\n  A->>B: x\n  end\n", TEXT, false},
    {"sequence title", "sequenceDiagram\n  title ", "\n  A->>B: x\n", TEXT, false},
    {"class label", "classDiagram\n  class A[", "]\n", STR, true},
    {"class note", "classDiagram\n  class A\n  note for A ", "\n", STR, true},
    {"class relation", "classDiagram\n  A --> B : ", "\n", TEXT, true},
    {"state description", "stateDiagram-v2\n  A --> B\n  A : ", "\n", TEXT, true},
    {"state alias", "stateDiagram-v2\n  state ", " as A\n  A --> B\n", STR, true},
    {"state transition", "stateDiagram-v2\n  A --> B : ", "\n", TEXT, true},
    {"state note", "stateDiagram-v2\n  A --> B\n  note right of A : ", "\n", TEXT, true},
    {"er relation", "erDiagram\n  A ||--o{ B : ", "\n", STR, false},
    {"er comment", "erDiagram\n  A {\n    string name ", "\n  }\n", STR, false},
    {"er alias", "erDiagram\n  p[", "] {\n    string x\n  }\n", STR, false},
    {"pie slice", "pie\n  ", " : 3\n  \"b\" : 4\n", STR, false},
    {"pie title", "pie title ", "\n  \"a\" : 3\n", TEXT, false},
    {"gantt task", "gantt\n  dateFormat YYYY-MM-DD\n  section S\n  ", " :a1, 2026-01-01, 3d\n", TEXT, false},
    {"gantt section", "gantt\n  dateFormat YYYY-MM-DD\n  section ", "\n  t :a1, 2026-01-01, 3d\n", TEXT, false},
    {"gantt title", "gantt\n  title ", "\n  dateFormat YYYY-MM-DD\n  t :a1, 2026-01-01, 3d\n", TEXT, false},
    {"journey task", "journey\n  section S\n    ", ": 5: Me\n", TEXT, true},
    {"journey section", "journey\n  section ", "\n    t: 5: Me\n", TEXT, true},
    {"journey actor", "journey\n  section S\n    t: 5: ", "\n", TEXT, false},
    {"timeline event", "timeline\n  2002 : ", "\n", TEXT, true},
    {"timeline period", "timeline\n  ", " : x\n", TEXT, true},
    {"timeline section", "timeline\n  section ", "\n  2002 : x\n", TEXT, true},
    {"mindmap node", "mindmap\n  root\n    a[", "]\n", STR, true},
    {"quadrant point", "quadrantChart\n  ", ": [0.3, 0.6]\n", STR, false},
    {"quadrant label", "quadrantChart\n  quadrant-1 ", "\n  A: [0.3, 0.6]\n", TEXT, true},
    {"quadrant axis", "quadrantChart\n  x-axis ", " --> High\n  A: [0.3, 0.6]\n", TEXT, false},
    {"quadrant title", "quadrantChart\n  title ", "\n  A: [0.3, 0.6]\n", TEXT, false},
    {"xychart title", "xychart\n  title ", "\n  bar [1, 2]\n", STR, false},
    {"xychart category", "xychart\n  x-axis [", ", b]\n  bar [1, 2]\n", STR, false},
    {"xychart axis title", "xychart\n  x-axis ", " [a, b]\n  bar [1, 2]\n", STR, false},
    {"xychart series", "xychart\n  x-axis [a, b]\n  bar ", " [1, 2]\n", STR, false},
    {"gitGraph commit id", "gitGraph\n  commit id: ", "\n", STR, false},
    {"gitGraph tag", "gitGraph\n  commit tag: ", "\n", STR, false},
    {"sankey node", "sankey-beta\n", ",B,3\n", CSV, false},
    {"block node", "block-beta\n  a[", "] b\n", STR, false},
    {"block edge", "block-beta\n  a b\n  a -- ", " --> b\n", STR, false},
    {"requirement text", "requirementDiagram\n  requirement r {\n  id: 1\n  text: ", "\n  }\n", STR, false},
    {"requirement type", "requirementDiagram\n  element e {\n  type: ", "\n  }\n", TEXT, false},
    {"C4 label", "C4Context\n  Person(p, ", ", \"d\")\n", STR, true},
    {"C4 description", "C4Context\n  Person(p, \"x\", ", ")\n", STR, true},
    {"C4 relation", "C4Context\n  Person(p, \"x\")\n  System(s, \"y\")\n  Rel(p, s, ", ")\n", STR, false},
    {"C4 title", "C4Context\n  title ", "\n  Person(p, \"x\")\n", TEXT, false},
    {"architecture service", "architecture-beta\n  service s(server)[", "]\n", TEXT, false},
    {"architecture group", "architecture-beta\n  group g(cloud)[", "]\n  service s(server)[x] in g\n", TEXT, false},
    {"architecture edge", "architecture-beta\n  service a(server)[x]\n  service b(server)[y]\n  a:R -[", "]- L:b\n", TEXT, false},
    {"packet field", "packet-beta\n0-15: ", "\n", STR, false},
    {"packet title", "packet-beta\ntitle ", "\n0-15: \"x\"\n", TEXT, false},
    {"kanban card", "kanban\n  Todo\n    c1[", "]\n", STR, true},
    {"kanban column", "kanban\n  c0[", "]\n    c1[x]\n", STR, true},
    {"radar axis", "radar-beta\n  axis a[", "], b[\"y\"], c[\"z\"]\n  curve k[\"K\"]{1,2,3}\n", STR, false},
    {"radar curve", "radar-beta\n  axis a[\"x\"], b[\"y\"], c[\"z\"]\n  curve k[", "]{1,2,3}\n", STR, false},
    {"radar title", "radar-beta\n  title ", "\n  axis a[\"x\"], b[\"y\"], c[\"z\"]\n  curve k[\"K\"]{1,2,3}\n", TEXT, false},
    {"treemap leaf", "treemap-beta\n\"S\"\n    ", ": 5\n    \"o\": 3\n", STR, false},
    {"treemap section", "treemap-beta\n", "\n    \"l\": 5\n", STR, false},
    {"zenuml message", "zenuml\n  A->B: ", "\n", TEXT, false},
    {"zenuml title", "zenuml\n  title ", "\n  A->B: x\n", TEXT, false},
    {"ishikawa head", "ishikawa-beta\n    ", "\n    Process\n        x\n", TEXT, true},
    {"ishikawa cause", "ishikawa-beta\n    H\n    Process\n        ", "\n", TEXT, true},
    {"cynefin item", "cynefin-beta\n  complex\n    ", "\n", STR, true},
    {"cynefin transition", "cynefin-beta\n  complex\n    \"x\"\n  complex --> clear : ", "\n", STR, false},
    {"cynefin title", "cynefin-beta\n  title ", "\n  complex\n    \"x\"\n", TEXT, false},
    {"venn set", "venn-beta\n  set A[", "]\n  set B[\"y\"]\n", STR, true},
    {"venn text", "venn-beta\n  set A[\"x\"]\n    text A1[", "]\n", STR, true},
    {"treeView description", "treeView-beta\n    root/\n        f.txt ## ", "\n", TEXT, false},
    {"eventmodeling data", "eventmodeling\ntf 01 evt E [[D]]\n\ndata D {\n  a: ", "\n}\n", TEXT, false},
    {"railroad terminal", "railroad-ebnf-beta\nr = ", " ;\n", STR, false},
    {"railroad fn terminal", "railroad-beta\nr = terminal(", ") ;\n", STR, false},
    {"wardley component", "wardley-beta\ncomponent ", " [0.5, 0.5]\n", TEXT, false},
    {"wardley note", "wardley-beta\ncomponent X [0.5, 0.5]\nnote ", " [0.3, 0.4]\n", STR, false},
    {"wardley title", "wardley-beta\ntitle ", "\ncomponent X [0.5, 0.5]\n", TEXT, false},
    {"flowchart id", "flowchart LR\n  ", " --> B\n", ID, false},
    {"sequence participant", "sequenceDiagram\n  ", "->>B: x\n", ID, false},
    {"class name", "classDiagram\n  class ", "\n", ID, false},
    {"state id", "stateDiagram-v2\n  ", " --> B\n", ID, false},
    {"er entity", "erDiagram\n  ", " ||--o{ B : r\n", ID, false},
    {"gitGraph branch", "gitGraph\n  commit\n  branch ", "\n  commit\n", ID, false},
    {"mindmap id", "mindmap\n  root\n    ", "\n", ID, false},
    {"treeView node", "treeView-beta\n    root/\n        ", "\n", ID, false},
    {"eventmodeling name", "eventmodeling\ntf 01 evt ", "\n", ID, false},
};

static const char *const PIECES[] = {
    "[", "]", "\"", "#", ";", ":", ",", "(", ")", "|", "{", "}", "<", ">", "&", "%", "'", "`", "\\", "=", "-",
    "/", "*", "_", "~", "@", "!", "$", "^", "+", ".", "?", " ", "\t", "\n", "中文", "é", "😀", "#35;",
    "#quot;", "%%", "<br>", "-->", "fa:fa-car", "::", ":::", "--", "..", "a1",
};

static const char *const IDS[] = {"_", "0", "9", "x", "A", "中文", "é", "😀"};

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static unsigned rnd(unsigned n) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (unsigned)(rng % n);
}

/* text content of the SVG (outside <style>/<title>/<desc>), entities decoded, whitespace dropped */
static size_t svg_text(const char *svg, size_t cap, char out[static cap]) {
    size_t k = 0;
    for (const char *p = svg; *p && k + 8 < cap;) {
        if (*p == '<') {
            static const char *const SKIP[] = {"<style", "<title", "<desc"};
            const char *e = nullptr;
            for (size_t i = 0; i < 3; i++)
                if (!strncmp(p, SKIP[i], strlen(SKIP[i]))) e = strstr(p, i == 0 ? "</style>" : i == 1 ? "</title>" : "</desc>");
            p = e ? e : strchr(p, '>');
            if (!p) break;
            p++;
            continue;
        }
        unsigned long cp = (unsigned char)*p++;
        if (cp == '&') {
            const char *semi = strchr(p, ';');
            if (!semi) break;
            if (*p == '#') cp = strtoul(p + 1 + (p[1] == 'x'), nullptr, p[1] == 'x' ? 16 : 10);
            else cp = !strncmp(p, "lt;", 3) ? '<' : !strncmp(p, "gt;", 3) ? '>' : !strncmp(p, "amp;", 4) ? '&'
                    : !strncmp(p, "quot;", 5) ? '"' : '\'';
            p = semi + 1;
            if (cp >= 0x80) { /* re-encode as UTF-8 */
                if (cp < 0x800) out[k++] = (char)(0xC0 | cp >> 6);
                else {
                    if (cp < 0x10000) out[k++] = (char)(0xE0 | cp >> 12);
                    else out[k++] = (char)(0xF0 | cp >> 18), out[k++] = (char)(0x80 | (cp >> 12 & 63));
                    out[k++] = (char)(0x80 | (cp >> 6 & 63));
                }
                out[k++] = (char)(0x80 | (cp & 63));
                continue;
            }
        }
        if (cp > ' ') out[k++] = (char)cp;
    }
    out[k] = 0;
    return k;
}

static void squeeze(char *s) {
    char *o = s;
    for (; *s; s++) if ((unsigned char)*s > ' ') *o++ = *s;
    *o = 0;
}

static char svgt[1 << 20];

/* 0 ok, 1 refused as expected, -1 failure */
static int one(const Case c[static 1], const char *label, bool verbose) {
    mmd_writer *w = mmd_writer_new();
    mmd_w_raw(w, c->pre);
    if (c->ctx == STR) mmd_w_str(w, label, c->br ? MMD_W_BR : 0);
    else if (c->ctx == TEXT) mmd_w_text(w, label, c->br ? MMD_W_BR : 0);
    else if (c->ctx == CSV) mmd_w_csv(w, label);
    else mmd_w_id(w, label);
    mmd_w_raw(w, c->post);
    mmd_buffer svg = {};
    mmd_error e = {};
    mmd_status st = mmd_writer_render_svg(w, nullptr, &svg, &e);
    const char *src;
    size_t n;
    bool refused = mmd_writer_text(w, &src, &n, nullptr) != MMD_OK;
    int r = 0;
    if (refused) {
        r = strchr(label, '\n') && !c->br ? 1 : -1;
    } else if (st) {
        r = -1;
    } else {
        char want[256];
        snprintf(want, sizeof want, "%s", label);
        squeeze(want);
        svg_text(svg.data, sizeof svgt, svgt);
        r = strstr(svgt, want) ? 0 : -1;
    }
    if (r < 0 && verbose) {
        printf("FAIL %s: label \"", c->name);
        for (const char *p = label; *p; p++) printf(*p == '\n' ? "\\n" : "%c", *p);
        printf("\": %s\n", refused ? "refused" : st ? e.message : "not in the SVG");
        if (!refused) printf("---\n%.*s---\n", (int)n, src);
    }
    mmd_buffer_free(&svg);
    mmd_writer_free(w);
    return r;
}

static int roundtrip(unsigned iters) {
    int bad = 0;
    unsigned refused = 0, total = 0;
    for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        const Case *c = &CASES[i];
        int cbad = 0;
        for (unsigned it = 0; it < iters && cbad < 3; it++) {
            char label[128] = "";
            for (unsigned part = 0; part < 3; part++) {
                for (unsigned k = rnd(3); k--;) {
                    strcat(label, c->ctx == ID ? IDS[rnd(sizeof IDS / sizeof *IDS)] : PIECES[rnd(sizeof PIECES / sizeof *PIECES)]);
                }
                if (part < 2) strcat(label, part ? "Wk" : "Zq");
            }
            int r = one(c, label, true);
            total++;
            if (r > 0) refused++;
            if (r < 0) cbad++;
        }
        bad += cbad;
    }
    printf("writer round trip: %u labels in %zu slots, %u refused (line break), %d failed\n", total,
           sizeof CASES / sizeof *CASES, refused, bad);
    return bad;
}

static void rnd_label(char out[static 64], bool nl) {
    out[0] = 0;
    for (unsigned k = rnd(4); k--;) {
        const char *pc = PIECES[rnd(sizeof PIECES / sizeof *PIECES)];
        if (nl || strcmp(pc, "\n")) strcat(out, pc);
    }
}

static bool shows(const mmd_writer *w, size_t n, char labels[][64]) {
    mmd_buffer svg = {};
    mmd_error e;
    if (mmd_writer_render_svg(w, nullptr, &svg, &e)) {
        printf("FAIL typed: %s\n", e.message);
        return false;
    }
    svg_text(svg.data, sizeof svgt, svgt);
    mmd_buffer_free(&svg);
    for (size_t i = 0; i < n; i++) {
        char want[64];
        snprintf(want, sizeof want, "%s", labels[i]);
        squeeze(want);
        if (!strstr(svgt, want)) {
            const char *src;
            size_t len;
            (void)mmd_writer_text(w, &src, &len, nullptr);
            printf("FAIL typed: \"%s\" not in the SVG of\n%s", labels[i], src);
            return false;
        }
    }
    return true;
}

/* the typed helpers: every shape, labels and categories built from the same nasty pieces */
static int typed(unsigned iters) {
    int bad = 0;
    char L[32][64];
    mmd_writer *w = mmd_writer_new();
    for (unsigned it = 0; it < iters; it++) {
        mmd_writer_reset(w);
        mmd_flow_begin(w, "LR");
        size_t n = 0;
        for (unsigned sh = MMD_SHAPE_RECT; sh <= MMD_SHAPE_TRAP_ALT; sh++) {
            char id[16];
            snprintf(id, sizeof id, "n%u", sh);
            rnd_label(L[n], true);
            snprintf(L[n] + strlen(L[n]), 8, "Zq%u", sh);
            mmd_flow_node(w, id, L[n++], (mmd_shape)sh);
            if (sh) {
                char prev[16];
                snprintf(prev, sizeof prev, "n%u", sh - 1);
                rnd_label(L[n], true);
                snprintf(L[n] + strlen(L[n]), 8, "Wk%u", sh);
                mmd_flow_edge(w, prev, id, L[n++]);
            }
        }
        bad += !shows(w, n, L);

        mmd_writer_reset(w);
        rnd_label(L[0], false);
        strcat(L[0], "Ttl");
        mmd_xy_begin(w, L[0], it & 1);
        const char *cats[4];
        for (size_t k = 0; k < 4; k++) {
            rnd_label(L[1 + k], false);
            snprintf(L[1 + k] + strlen(L[1 + k]), 8, "C%zu", k);
            cats[k] = L[1 + k];
        }
        rnd_label(L[5], false), strcat(L[5], "Xax");
        rnd_label(L[6], false), strcat(L[6], "Yax");
        rnd_label(L[7], false), strcat(L[7], "Bar");
        mmd_xy_x_cats(w, L[5], 4, cats);
        mmd_xy_y_range(w, L[6], 0, 1e-3 * (it + 1));
        double v[4] = {1e-4 * it, 3.25e-4, 1e-7, 7.5e-4};
        mmd_xy_bar(w, L[7], 4, v);
        mmd_xy_line(w, nullptr, 4, v);
        bad += !shows(w, 8, L);

        mmd_writer_reset(w);
        rnd_label(L[0], false), strcat(L[0], "Pie");
        mmd_pie_begin(w, L[0], it & 1);
        for (size_t k = 1; k < 4; k++) {
            rnd_label(L[k], false);
            snprintf(L[k] + strlen(L[k]), 8, "S%zu", k);
            mmd_pie_slice(w, L[k], 0.125 * (double)k);
        }
        bad += !shows(w, 4, L);
    }
    mmd_writer_free(w);
    printf("writer typed helpers: %u flowchart/xychart/pie diagrams, %d failed\n", 3 * iters, bad);
    return bad;
}

/* shortest round trip: reads back exactly, and no %.Ng with fewer significant digits does */
static int numbers(unsigned iters) {
    int bad = 0;
    mmd_writer *w = mmd_writer_new();
    for (unsigned it = 0; it < iters; it++) {
        double v;
        switch (it % 4) {
        case 0: v = (double)rnd(100000) / 100; break;                        /* typical data */
        case 1: v = ldexp((double)(rng >> 11), (int)rnd(200) - 150) * (rnd(2) ? 1 : -1); break; /* any magnitude */
        case 2: v = (double)rnd(1000) * pow(10, (int)rnd(40) - 20); break;
        default: memcpy(&v, &rng, sizeof v); if (!isfinite(v)) continue; rnd(2); break;    /* raw bits */
        }
        mmd_writer_reset(w);
        mmd_w_num(w, v);
        const char *s;
        size_t n;
        if (mmd_writer_text(w, &s, &n, nullptr) || strchr(s, 'e') || strtod(s, nullptr) != v) {
            printf("FAIL number %.17g -> %s\n", v, s ? s : "(error)");
            bad++;
            continue;
        }
        char dig[400];
        size_t nd = 0;
        for (const char *d = s; *d; d++) if (*d >= '0' && *d <= '9' && (nd || *d != '0')) dig[nd++] = *d;
        while (nd && dig[nd - 1] == '0') nd--;
        int sig = (int)nd; /* significant digits written */
        char t[40];
        snprintf(t, sizeof t, "%.*g", sig - 1, v);
        if (v != 0 && sig > 1 && strtod(t, nullptr) == v) {
            printf("FAIL number %.17g -> %s is not the shortest (%s)\n", v, s, t);
            bad++;
        }
    }
    mmd_writer_free(w);
    printf("writer numbers: %u values, %d failed\n", iters, bad);
    return bad;
}

/* the error sticks: later calls are no-ops, line:col points at the rejected value, reset clears it */
enum { NBAD = 16 };
static void bad_call(mmd_writer *w, int k) {
    switch (k) {
    case 0: mmd_w_id(w, "a b"); break;
    case 1: mmd_w_id(w, ""); break;
    case 2: mmd_w_id(w, "a-b"); break;
    case 3: mmd_w_num(w, NAN); break;
    case 4: mmd_w_num(w, -INFINITY); break;
    case 5: mmd_w_str(w, "a\nb", 0); break;
    case 6: mmd_w_text(w, "a\x01", MMD_W_BR); break;
    case 7: mmd_w_str(w, "\xff", 0); break;
    case 8: mmd_w_text(w, "\xed\xa0\x80", 0); break; /* surrogate */
    case 9: mmd_w_str(w, "\xc0\xaf", 0); break;       /* overlong '/' */
    case 10: mmd_w_csv(w, "a\nb"); break;
    case 11: mmd_flow_begin(w, "XY"); break;
    case 12: mmd_flow_edge(w, "a", "end", nullptr); break;
    case 13: mmd_pie_slice(w, "a", -1); break;
    case 14: mmd_flow_node(w, "a", nullptr, (mmd_shape)99); break;
    case 15: mmd_xy_bar(w, nullptr, 1, (const double[]){NAN}); break;
    }
}
/* the host's LC_NUMERIC must leak into neither the text nor the messages */
static int comma_locale(void) {
    static const double V[] = {1.5, 1e-7, -2.5e-30, 1.25e30, 0x1p60};
    char out[2][512] = {};
    for (int pass = 0; pass < 2; pass++) {
        if (pass && !setlocale(LC_NUMERIC, "de_DE.UTF-8") && !setlocale(LC_NUMERIC, "fr_FR.UTF-8")) return 0;
        mmd_writer *w = mmd_writer_new();
        for (size_t i = 0; i < sizeof V / sizeof *V; i++) mmd_w_num(w, V[i]), mmd_w_raw(w, " ");
        const char *s;
        size_t n;
        mmd_error e;
        if (mmd_writer_text(w, &s, &n, nullptr)) s = "";
        int k = snprintf(out[pass], sizeof out[pass], "%s| ", s);
        mmd_writer_reset(w);
        mmd_pie_slice(w, "a", -1.5);
        (void)mmd_writer_text(w, &s, &n, &e);
        snprintf(out[pass] + k, sizeof out[pass] - (size_t)k, "%s", e.message);
        mmd_writer_free(w);
    }
    setlocale(LC_NUMERIC, "C");
    if (!strstr(out[0], "0.0000001 ") || !strstr(out[0], "-1.5") || strcmp(out[0], out[1]))
        return printf("FAIL comma locale:\n  %s\n  %s\n", out[0], out[1]), 1;
    return 0;
}

static int errors(void) {
    int bad = 0;
    mmd_writer *w = mmd_writer_new();
    for (int k = 0; k < NBAD; k++) {
        mmd_writer_reset(w);
        mmd_w_raw(w, "pie\n  ");
        bad_call(w, k);
        mmd_w_raw(w, "more");
        const char *s;
        size_t n;
        mmd_error e = {};
        mmd_buffer svg = {};
        if (mmd_writer_text(w, &s, &n, &e) != MMD_ERR_SYNTAX || s || e.line < 2 || (e.col != 3 && k != 12 && k != 15) ||
            mmd_writer_render_svg(w, nullptr, &svg, nullptr) != MMD_ERR_SYNTAX) {
            printf("FAIL error case %d: %u:%u %s\n", k, e.line, e.col, e.message);
            bad++;
        }
        mmd_writer_reset(w);
        mmd_w_raw(w, "x");
        if (mmd_writer_text(w, &s, &n, nullptr) || strcmp(s, "x")) printf("FAIL reset after case %d\n", k), bad++;
    }
    mmd_writer_free(w);
    /* a failed mmd_writer_new() is just an out-of-memory writer */
    mmd_w_str(nullptr, "x", 0);
    const char *s;
    size_t n;
    if (mmd_writer_text(nullptr, &s, &n, nullptr) != MMD_ERR_NOMEM) printf("FAIL null writer\n"), bad++;
    bad += comma_locale();
    printf("writer errors: %d cases, %d failed\n", NBAD, bad);
    return bad;
}

int main(int argc, char **argv) {
    unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 200;
    int bad = roundtrip(iters) + typed(iters / 4) + numbers(iters * 50) + errors();
    return bad != 0;
}
