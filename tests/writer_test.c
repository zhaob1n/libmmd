/* mmd_writer round trip: random labels full of mermaid syntax go through each escape context into every
 * diagram type; the rendered SVG must show them verbatim, or the writer must have refused them. */
#include <ctype.h>
#include <math.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmd.h"

#define countof(a) (sizeof(a) / sizeof *(a))

typedef enum Ctx { STR, TEXT, CSV, ID } Ctx;
typedef struct Case {
    const char *name, *pre, *post;
    Ctx ctx;
    unsigned flags; /* MMD_W_BR: the diagram draws line breaks here; MMD_W_LINESTART: text that starts
                       a statement, so labels may start with a keyword */
} Case;

static const Case CASES[] = {
    {"flowchart node", "flowchart LR\n  A[", "] --> B\n", STR, MMD_W_BR},
    {"flowchart round", "flowchart LR\n  A(", ") --> B\n", STR, MMD_W_BR},
    {"flowchart rhombus", "flowchart LR\n  A{", "} --> B\n", STR, MMD_W_BR},
    {"flowchart @{label}", "flowchart LR\n  A@{ shape: rect, label: ", " } --> B\n", STR, MMD_W_BR},
    {"flowchart edge |l|", "flowchart LR\n  A -->|", "| B\n", STR, MMD_W_BR},
    {"flowchart edge -- l -->", "flowchart LR\n  A -- ", " --> B\n", STR, MMD_W_BR},
    {"flowchart subgraph", "flowchart LR\n  subgraph s1 [", "]\n    A\n  end\n", STR, MMD_W_BR},
    {"swimlane node", "swimlane-beta LR\n  subgraph L\n    A[", "]\n  end\n", STR, MMD_W_BR},
    {"swimlane lane", "swimlane-beta LR\n  subgraph l1 [", "]\n    A\n  end\n", STR, MMD_W_BR},
    {"sequence message", "sequenceDiagram\n  A->>B: ", "\n", TEXT, MMD_W_BR},
    {"sequence note", "sequenceDiagram\n  A->>B: x\n  Note over A: ", "\n", TEXT, MMD_W_BR},
    {"sequence alias", "sequenceDiagram\n  participant A as ", "\n  A->>B: x\n", TEXT, MMD_W_BR},
    {"sequence loop", "sequenceDiagram\n  loop ", "\n  A->>B: x\n  end\n", TEXT, 0},
    {"sequence title", "sequenceDiagram\n  title ", "\n  A->>B: x\n", TEXT, 0},
    {"class label", "classDiagram\n  class A[", "]\n", STR, MMD_W_BR},
    {"class note", "classDiagram\n  class A\n  note for A ", "\n", STR, MMD_W_BR},
    {"class relation", "classDiagram\n  A --> B : ", "\n", TEXT, MMD_W_BR},
    {"state description", "stateDiagram-v2\n  A --> B\n  A : ", "\n", TEXT, MMD_W_BR},
    {"state alias", "stateDiagram-v2\n  state ", " as A\n  A --> B\n", STR, MMD_W_BR},
    {"state transition", "stateDiagram-v2\n  A --> B : ", "\n", TEXT, MMD_W_BR},
    {"state note", "stateDiagram-v2\n  A --> B\n  note right of A : ", "\n", TEXT, MMD_W_BR},
    {"er relation", "erDiagram\n  A ||--o{ B : ", "\n", STR, 0},
    {"er comment", "erDiagram\n  A {\n    string name ", "\n  }\n", STR, 0},
    {"er alias", "erDiagram\n  p[", "] {\n    string x\n  }\n", STR, 0},
    {"pie slice", "pie\n  ", " : 3\n  \"b\" : 4\n", STR, 0},
    {"pie title", "pie title ", "\n  \"a\" : 3\n", TEXT, 0},
    {"gantt task", "gantt\n  dateFormat YYYY-MM-DD\n  section S\n  ", " :a1, 2026-01-01, 3d\n", TEXT, MMD_W_LINESTART},
    {"gantt section", "gantt\n  dateFormat YYYY-MM-DD\n  section ", "\n  t :a1, 2026-01-01, 3d\n", TEXT, 0},
    {"gantt title", "gantt\n  title ", "\n  dateFormat YYYY-MM-DD\n  t :a1, 2026-01-01, 3d\n", TEXT, 0},
    {"journey task", "journey\n  section S\n    ", ": 5: Me\n", TEXT, MMD_W_BR | MMD_W_LINESTART},
    {"journey section", "journey\n  section ", "\n    t: 5: Me\n", TEXT, MMD_W_BR},
    {"journey actor", "journey\n  section S\n    t: 5: ", "\n", TEXT, 0},
    {"timeline event", "timeline\n  2002 : ", "\n", TEXT, MMD_W_BR},
    {"timeline period", "timeline\n  ", " : x\n", TEXT, MMD_W_BR | MMD_W_LINESTART},
    {"timeline section", "timeline\n  section ", "\n  2002 : x\n", TEXT, MMD_W_BR},
    {"mindmap node", "mindmap\n  root\n    a[", "]\n", STR, MMD_W_BR},
    {"quadrant point", "quadrantChart\n  ", ": [0.3, 0.6]\n", STR, 0},
    {"quadrant label", "quadrantChart\n  quadrant-1 ", "\n  A: [0.3, 0.6]\n", STR, 0},
    {"quadrant axis", "quadrantChart\n  x-axis ", " --> High\n  A: [0.3, 0.6]\n", STR, 0},
    {"quadrant title", "quadrantChart\n  title ", "\n  A: [0.3, 0.6]\n", TEXT, 0},
    {"xychart title", "xychart\n  title ", "\n  bar [1, 2]\n", STR, 0},
    {"xychart category", "xychart\n  x-axis [", ", b]\n  bar [1, 2]\n", STR, 0},
    {"xychart axis title", "xychart\n  x-axis ", " [a, b]\n  bar [1, 2]\n", STR, 0},
    {"xychart series", "xychart\n  x-axis [a, b]\n  bar ", " [1, 2]\n", STR, 0},
    {"gitGraph commit id", "gitGraph\n  commit id: ", "\n", STR, 0},
    {"gitGraph tag", "gitGraph\n  commit tag: ", "\n", STR, 0},
    {"sankey node", "sankey-beta\n", ",B,3\n", CSV, 0},
    {"block node", "block-beta\n  a[", "] b\n", STR, 0},
    {"block edge", "block-beta\n  a b\n  a -- ", " --> b\n", STR, 0},
    {"requirement text", "requirementDiagram\n  requirement r {\n  id: 1\n  text: ", "\n  }\n", STR, 0},
    {"requirement type", "requirementDiagram\n  element e {\n  type: ", "\n  }\n", STR, 0},
    {"C4 label", "C4Context\n  Person(p, ", ", \"d\")\n", STR, MMD_W_BR},
    {"C4 description", "C4Context\n  Person(p, \"x\", ", ")\n", STR, MMD_W_BR},
    {"C4 relation", "C4Context\n  Person(p, \"x\")\n  System(s, \"y\")\n  Rel(p, s, ", ")\n", STR, 0},
    {"C4 title", "C4Context\n  title ", "\n  Person(p, \"x\")\n", TEXT, 0},
    {"architecture service", "architecture-beta\n  service s(server)[", "]\n", TEXT, 0},
    {"architecture group", "architecture-beta\n  group g(cloud)[", "]\n  service s(server)[x] in g\n", TEXT, 0},
    {"architecture edge", "architecture-beta\n  service a(server)[x]\n  service b(server)[y]\n  a:R -[", "]- L:b\n", TEXT, 0},
    {"packet field", "packet-beta\n0-15: ", "\n", STR, 0},
    {"packet title", "packet-beta\ntitle ", "\n0-15: \"x\"\n", TEXT, 0},
    {"kanban card", "kanban\n  Todo\n    c1[", "]\n", STR, MMD_W_BR},
    {"kanban column", "kanban\n  c0[", "]\n    c1[x]\n", STR, MMD_W_BR},
    {"radar axis", "radar-beta\n  axis a[", "], b[\"y\"], c[\"z\"]\n  curve k[\"K\"]{1,2,3}\n", STR, 0},
    {"radar curve", "radar-beta\n  axis a[\"x\"], b[\"y\"], c[\"z\"]\n  curve k[", "]{1,2,3}\n", STR, 0},
    {"radar title", "radar-beta\n  title ", "\n  axis a[\"x\"], b[\"y\"], c[\"z\"]\n  curve k[\"K\"]{1,2,3}\n", TEXT, 0},
    {"treemap leaf", "treemap-beta\n\"S\"\n    ", ": 5\n    \"o\": 3\n", STR, 0},
    {"treemap section", "treemap-beta\n", "\n    \"l\": 5\n", STR, 0},
    {"zenuml message", "zenuml\n  A->B: ", "\n", TEXT, 0},
    {"zenuml title", "zenuml\n  title ", "\n  A->B: x\n", TEXT, 0},
    {"ishikawa head", "ishikawa-beta\n    ", "\n    Process\n        x\n", TEXT, MMD_W_BR | MMD_W_LINESTART},
    {"ishikawa cause", "ishikawa-beta\n    H\n    Process\n        ", "\n", TEXT, MMD_W_BR | MMD_W_LINESTART},
    {"cynefin item", "cynefin-beta\n  complex\n    ", "\n", STR, 0},
    {"cynefin transition", "cynefin-beta\n  complex\n    \"x\"\n  complex --> clear : ", "\n", STR, 0},
    {"cynefin title", "cynefin-beta\n  title ", "\n  complex\n    \"x\"\n", TEXT, 0},
    {"venn set", "venn-beta\n  set A[", "]\n  set B[\"y\"]\n", STR, 0},
    {"venn text", "venn-beta\n  set A[\"x\"]\n    text A1[", "]\n", STR, 0},
    {"treeView description", "treeView-beta\n    root/\n        f.txt ## ", "\n", TEXT, 0},
    {"eventmodeling data", "eventmodeling\ntf 01 evt E [[D]]\n\ndata D {\n  a: ", "\n}\n", TEXT, 0},
    {"railroad terminal", "railroad-ebnf-beta\nr = ", " ;\n", STR, 0},
    {"railroad fn terminal", "railroad-beta\nr = terminal(", ") ;\n", STR, 0},
    {"wardley component", "wardley-beta\ncomponent ", " [0.5, 0.5]\n", TEXT, MMD_W_LINESTART},
    {"wardley note", "wardley-beta\ncomponent X [0.5, 0.5]\nnote ", " [0.3, 0.4]\n", STR, 0},
    {"wardley title", "wardley-beta\ntitle ", "\ncomponent X [0.5, 0.5]\n", TEXT, 0},
    {"flowchart id", "flowchart LR\n  ", " --> B\n", ID, 0},
    {"sequence participant", "sequenceDiagram\n  ", "->>B: x\n", ID, 0},
    {"class name", "classDiagram\n  class ", "\n", ID, 0},
    {"state id", "stateDiagram-v2\n  ", " --> B\n", ID, 0},
    {"er entity", "erDiagram\n  ", " ||--o{ B : r\n", ID, 0},
    {"gitGraph branch", "gitGraph\n  commit\n  branch ", "\n  commit\n", ID, 0},
    {"mindmap id", "mindmap\n  root\n    ", "\n", ID, 0},
    {"treeView node", "treeView-beta\n    root/\n        ", "\n", ID, 0},
    {"eventmodeling name", "eventmodeling\ntf 01 evt ", "\n", ID, 0},
};

static const char *const PIECES[] = {
    "[", "]", "\"", "#", ";", ":", ",", "(", ")", "|", "{", "}", "<", ">", "&", "%", "'", "`", "\\", "=", "-",
    "/", "*", "_", "~", "@", "!", "$", "^", "+", ".", "?", " ", "\t", "\n", "中文", "é", "😀", "#35;",
    "#quot;", "%%", "<br>", "-->", "fa:fa-car", "::", ":::", "--", "..", "a1",
};

/* statement keywords of the MMD_W_LINESTART slots' diagrams */
static const char *const LSKW[] = {"title ", "section ", "accTitle", "accDescr", "timeline", "journey", "ishikawa",
                                   "component ", "evolve", "anchor ", "note ", "pipeline", "dateFormat", "excludes", "click"};

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

/* --dump DIR: also save each diagram the test builds as DIR/NNNN.mmd, with the labels its SVG must
 * show (whitespace dropped, one per line) in NNNN.want, for tools/writer_mmdc_check.py */
static const char *dump_dir;
static unsigned ndump;
static void dump(const char *name, const char *src, size_t n, const char *const labels[static n]) {
    if (!dump_dir) return;
    char path[4096];
    snprintf(path, sizeof path, "%s/%04u.mmd", dump_dir, ndump);
    FILE *f = fopen(path, "w");
    if (!f) perror(path), exit(1);
    fputs(src, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/%04u.want", dump_dir, ndump++);
    if (!(f = fopen(path, "w"))) perror(path), exit(1);
    fprintf(f, "# %s\n", name);
    for (size_t i = 0; i < n; i++) {
        char want[256];
        snprintf(want, sizeof want, "%s", labels[i]);
        squeeze(want);
        fprintf(f, "%s\n", want);
    }
    fclose(f);
}

/* mermaid's sankey reads printable ASCII (no tab) without entities (#name;) or %%{, and can't show < > */
static bool csv_ok(const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80 || strchr("\n\t<>", *s) || !strncmp(s, "%%{", 3)) return false;
        if (*s == '#' && isalnum((unsigned char)s[1])) {
            const char *e = s + 1;
            while (isalnum((unsigned char)*e)) e++;
            if (*e == ';') return false;
        }
    }
    return true;
}

/* 0 ok, 1 refused as expected, -1 failure */
static int one(const Case c[static 1], const char *label, bool verbose) {
    mmd_writer *w = mmd_writer_new();
    mmd_w_raw(w, c->pre);
    if (c->ctx == STR) mmd_w_str(w, label, c->flags);
    else if (c->ctx == TEXT) mmd_w_text(w, label, c->flags);
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
        r = (strchr(label, '\n') && !(c->flags & MMD_W_BR)) || (c->ctx == CSV && !csv_ok(label)) ||
            (c->ctx == ID && strstr(label, "😀")) ? 1 : -1;
    } else if (st) {
        r = -1;
    } else {
        char want[256];
        snprintf(want, sizeof want, "%s", label);
        squeeze(want);
        svg_text(svg.data, sizeof svgt, svgt);
        r = strstr(svgt, want) ? 0 : -1;
        dump(c->name, src, 1, &label);
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
            if (c->flags & MMD_W_LINESTART && rnd(2)) strcat(label, LSKW[rnd(countof(LSKW))]);
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
    printf("writer round trip: %u labels in %zu slots, %u refused (line break, sankey, emoji id), %d failed\n", total,
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

static bool shows(const char *name, const mmd_writer *w, size_t n, char labels[][64]) {
    mmd_buffer svg = {};
    mmd_error e;
    if (dump_dir) {
        const char *src, *ls[32];
        size_t len;
        for (size_t i = 0; i < n; i++) ls[i] = labels[i];
        if (!mmd_writer_text(w, &src, &len, nullptr)) dump(name, src, n, ls);
    }
    if (mmd_writer_render_svg(w, nullptr, &svg, &e)) {
        const char *src;
        size_t len;
        printf("FAIL typed: %s in\n%s", e.message, mmd_writer_text(w, &src, &len, nullptr) ? "" : src);
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
        bad += !shows("flowchart helpers", w, n, L);

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
        bad += !shows("xychart helpers", w, 8, L);

        mmd_writer_reset(w);
        rnd_label(L[0], false), strcat(L[0], "Pie");
        mmd_pie_begin(w, L[0], it & 1);
        for (size_t k = 1; k < 4; k++) {
            rnd_label(L[k], false);
            snprintf(L[k] + strlen(L[k]), 8, "S%zu", k);
            mmd_pie_slice(w, L[k], 0.125 * (double)k);
        }
        bad += !shows("pie helpers", w, 4, L);
    }
    mmd_writer_free(w);
    printf("writer typed helpers: %u flowchart/xychart/pie diagrams, %d failed\n", 3 * iters, bad);
    return bad;
}

/* L[k] = random label + a unique tag, so each must show up on its own */
static const char *lab(char L[][64], size_t k, bool nl) {
    rnd_label(L[k], nl);
    snprintf(L[k] + strlen(L[k]), 8, "Q%zu", k);
    return L[k];
}
/* ids that collide with a keyword of some diagram, or with nothing */
static const char *const KWIDS[] = {"end", "Andrew", "loop1", "par", "optimal", "note", "Over", "x", "_9", "中文",
                                    "box", "destroy", "off", "section", "client", "one", "to", "u"};

/* the helpers of the other diagram types, same idea as typed() */
static int typed2(unsigned iters) {
    int bad = 0;
    char L[32][64];
    mmd_writer *w = mmd_writer_new();
    for (unsigned it = 0; it < iters; it++) {
        size_t n = 0;
        const char *a = KWIDS[rnd(countof(KWIDS))], *b = KWIDS[rnd(countof(KWIDS))];
        if (!strcmp(a, b)) b = "y";
        mmd_writer_reset(w);
        mmd_seq_begin(w, lab(L, n++, false), it & 1);
        mmd_seq_participant(w, a, nullptr, false);
        mmd_seq_participant(w, b, it & 2 ? lab(L, n++, true) : nullptr, it & 4);
        mmd_seq_message(w, a, b, (mmd_arrow)rnd(8), lab(L, n++, true));
        mmd_seq_block(w, "alt", lab(L, n++, false));
        mmd_seq_activate(w, b, true);
        mmd_seq_message(w, b, a, (mmd_arrow)rnd(8), lab(L, n++, true));
        mmd_seq_activate(w, b, false);
        mmd_seq_block(w, "else", lab(L, n++, false));
        mmd_seq_note(w, MMD_OVER, a, b, lab(L, n++, true));
        mmd_seq_block(w, "loop", lab(L, n++, false));
        mmd_seq_note(w, (mmd_side)rnd(2), a, nullptr, lab(L, n++, true));
        mmd_end(w);
        mmd_end(w);
        if (!(it & 2)) strcpy(L[n++], b);
        strcpy(L[n++], a);
        bad += !shows("sequence helpers", w, n, L);

        static const char *const GKW[] = {"section x", "title", "2026-01-01 go", "5% done", "call me", "click", "Weekday"};
        mmd_writer_reset(w), n = 0;
        mmd_gantt_begin(w, lab(L, n++, false));
        mmd_gantt_section(w, lab(L, n++, false));
        mmd_gantt_task(w, lab(L, n++, false), "t1", MMD_TASK_CRIT, "2026-02-27", 1 + rnd(5) * 0.5);
        snprintf(L[n], 64, "%s", GKW[rnd(countof(GKW))]);
        mmd_gantt_task(w, L[n++], "t2", MMD_TASK_DONE, "t1", 2);
        mmd_gantt_task(w, lab(L, n++, false), nullptr, MMD_TASK_ACTIVE, nullptr, 3);
        mmd_gantt_task(w, lab(L, n++, false), nullptr, MMD_TASK_MILESTONE, "t2", 0);
        bad += !shows("gantt helpers", w, n, L);

        mmd_writer_reset(w), n = 0;
        mmd_state_begin(w, it & 1 ? "LR" : nullptr);
        mmd_state_group(w, "G", lab(L, n++, true));
        mmd_state(w, "S1", lab(L, n++, true));
        mmd_state_transition(w, nullptr, "S1", lab(L, n++, true));
        mmd_end(w);
        mmd_state(w, "S2", nullptr);
        mmd_state_transition(w, "G", "S2", lab(L, n++, true));
        mmd_state_transition(w, "S2", nullptr, nullptr);
        mmd_state_note(w, (mmd_side)rnd(2), "S2", lab(L, n++, true));
        snprintf(L[n++], 64, "direction LR, %s", it & 1 ? "a" : "b");
        mmd_state(w, "S3", L[n - 1]);
        bad += !shows("state helpers", w, n, L);

        mmd_writer_reset(w), n = 0;
        mmd_sankey_begin(w);
        for (size_t k = 0; k < 3; k++)
            while (!csv_ok(lab(L, k, false))) {}
        n = 3;
        mmd_sankey_flow(w, L[0], L[1], 2.5);
        mmd_sankey_flow(w, L[1], L[2], 1);
        bad += !shows("sankey helpers", w, n, L);

        mmd_writer_reset(w), n = 0;
        mmd_mindmap_begin(w);
        mmd_mindmap_node(w, 0, "r", lab(L, n++, true), MMD_SHAPE_CIRCLE);
        mmd_mindmap_node(w, 1, "a", lab(L, n++, true), MMD_SHAPE_RECT);
        mmd_mindmap_node(w, 2, "b", lab(L, n++, true), MMD_SHAPE_HEXAGON);
        mmd_mindmap_node(w, 1, "c", lab(L, n++, true), MMD_SHAPE_ROUND);
        mmd_mindmap_node(w, 2, "plain", nullptr, MMD_SHAPE_RECT);
        strcpy(L[n++], "plain");
        bad += !shows("mindmap helpers", w, n, L);

        static const char *const TKW[] = {"title x", "accTitle", "├── a", "\"q\"", "'q'", "icon(x)", "a :::b", "d/"};
        mmd_writer_reset(w), n = 0;
        mmd_tree_begin(w);
        mmd_tree_node(w, 0, lab(L, n++, false), true, nullptr);
        const char *t0 = lab(L, n++, false), *t1 = lab(L, n++, false);
        mmd_tree_node(w, 1, t0, false, t1);
        snprintf(L[n], 64, "%s", TKW[rnd(countof(TKW))]);
        mmd_tree_node(w, 1, L[n++], it & 1, nullptr);
        mmd_tree_node(w, 2, lab(L, n++, false), false, nullptr);
        bad += !shows("treeView helpers", w, n, L);

        mmd_writer_reset(w), n = 0;
        mmd_er_begin(w);
        size_t e1 = n;
        snprintf(L[n++], 64, "%s-%s", a, b);
        mmd_er_entity(w, L[e1], nullptr);
        mmd_er_attr(w, "varchar(255)", "name", MMD_KEY_PK, lab(L, n++, false));
        mmd_er_attr(w, "string[]", "tags", 0, nullptr);
        mmd_end(w);
        mmd_er_entity(w, "E2", lab(L, n++, false));
        mmd_end(w);
        mmd_er_rel(w, L[e1], (mmd_card)rnd(4), "E2", (mmd_card)rnd(4), it & 1, lab(L, n++, false));
        strcpy(L[n++], "varchar(255)");
        bad += !shows("ER helpers", w, n, L);

        static const char *const BR[] = {"feature/x-1.0", "release-2", "a.b_c", "x"};
        mmd_writer_reset(w), n = 0;
        mmd_git_begin(w, it & 1 ? "TB" : nullptr);
        const char *c0 = lab(L, n++, false), *c1 = lab(L, n++, false);
        mmd_git_commit(w, c0, c1, (mmd_commit)rnd(3));
        snprintf(L[n++], 64, "%s", BR[rnd(countof(BR))]);
        mmd_git_branch(w, L[n - 1]);
        mmd_git_checkout(w, L[n - 1]);
        mmd_git_commit(w, lab(L, n++, false), nullptr, MMD_COMMIT_NORMAL);
        mmd_git_checkout(w, "main");
        const char *br = L[n - 2], *m = lab(L, n++, false);
        mmd_git_merge(w, br, m, nullptr, MMD_COMMIT_NORMAL);
        mmd_git_cherry_pick(w, L[n - 2]);
        bad += !shows("gitGraph helpers", w, n, L);
    }
    mmd_writer_free(w);
    printf("writer typed helpers: %u sequence/gantt/state/sankey/mindmap/treeView/ER/gitGraph sets, %d failed\n", iters, bad);
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
        int k = snprintf(t, sizeof t, "%.*g", sig - 1, v);
        if (v != 0 && sig > 1 && k < (int)sizeof t && strtod(t, nullptr) == v) {
            printf("FAIL number %.17g -> %s is not the shortest (%s)\n", v, s, t);
            bad++;
        }
    }
    mmd_writer_free(w);
    printf("writer numbers: %u values, %d failed\n", iters, bad);
    return bad;
}

/* the error sticks: later calls are no-ops, line:col points at the rejected value, reset clears it */
enum { NBAD = 30 };
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
    case 16: mmd_xy_x_cats(w, nullptr, 0, nullptr); break; /* mermaid's [] needs an item */
    case 17: mmd_xy_line(w, "l", 0, nullptr); break;
    case 18: mmd_end(w); break; /* nothing open */
    case 19: mmd_seq_block(w, "else", nullptr); break; /* outside alt */
    case 20: mmd_gantt_task(w, "t", "done", 0, nullptr, 1); break; /* id reads as a tag */
    case 21: mmd_gantt_task(w, "t", nullptr, 0, "2026-02-29", 1); break; /* no such day: an id starting with a digit */
    case 22: mmd_gantt_task(w, "t", nullptr, 0, nullptr, -1); break;
    case 23: mmd_state_transition(w, "state", "B", nullptr); break;
    case 24: mmd_git_branch(w, "feature/"); break;
    case 25: mmd_git_branch(w, "commit"); break;
    case 26: mmd_mindmap_node(w, 1, "a", nullptr, MMD_SHAPE_RECT); break; /* the first node is the root */
    case 27: mmd_er_attr(w, "string", "a", 0, nullptr); break;          /* outside an entity */
    case 28: mmd_sankey_flow(w, "a", "b", -1); break;
    case 29: mmd_seq_note(w, MMD_LEFT_OF, "a", "b", "x"); break; /* only "over" spans two */
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
        if (mmd_writer_text(w, &s, &n, &e) != MMD_ERR_SYNTAX || s || e.line < 2 || (e.col != 3 && k != 12 && k != 15 && k < 18) ||
            mmd_writer_render_svg(w, nullptr, &svg, nullptr) != MMD_ERR_SYNTAX) {
            printf("FAIL error case %d: %u:%u %s\n", k, e.line, e.col, e.message);
            bad++;
        }
        mmd_writer_reset(w);
        mmd_w_raw(w, "x");
        if (mmd_writer_text(w, &s, &n, nullptr) || strcmp(s, "x")) printf("FAIL reset after case %d\n", k), bad++;
    }
    mmd_writer_reset(w);
    /* a block left open is reported, and mmd_end() closes it */
    mmd_seq_begin(w, nullptr, false);
    mmd_seq_block(w, "loop", "l");
    const char *t;
    size_t tn;
    if (mmd_writer_text(w, &t, &tn, nullptr) != MMD_ERR_SYNTAX) printf("FAIL open block not reported\n"), bad++;
    mmd_seq_message(w, "a", "b", MMD_ARROW_SOLID, "m");
    mmd_end(w);
    if (mmd_writer_text(w, &t, &tn, nullptr)) printf("FAIL closed block still reported\n"), bad++;
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

/* mmd_w_config is a directive before the diagram; the render honours the keys */
static int config_round(void) {
    int bad = 0;
    mmd_writer *w = mmd_writer_new();
    mmd_w_config(w, "xyChart.showDataLabel", "true");
    mmd_w_config(w, "xyChart.showDataLabelOutsideBar", "true");
    mmd_w_config(w, "xyChart.barOverlap", "true");
    mmd_xy_begin(w, "Groups", true);
    const char *cats[] = {"a", "b"};
    mmd_xy_x_cats(w, nullptr, 2, cats);
    mmd_xy_bar(w, "g1", 2, (double[]){3, 0});
    mmd_xy_bar(w, "g2", 2, (double[]){0, 4});
    const char *s;
    size_t n;
    mmd_error e = {};
    if (mmd_writer_text(w, &s, &n, &e) || !strstr(s, "%%{init: {\"xyChart\":{\"showDataLabel\":\"true\"}}}%%") ||
        !strstr(s, "barOverlap")) {
        printf("FAIL config text: %s\n", s ? s : e.message);
        bad++;
    }
    mmd_buffer svg = {};
    if (mmd_writer_render_svg(w, nullptr, &svg, &e)) {
        printf("FAIL config render: %s\n", e.message);
        bad++;
    } else {
        if (!strstr(svg.data, ">3</tspan>") || !strstr(svg.data, ">4</tspan>"))
            printf("FAIL config svg missing values\n%s\n", svg.data), bad++;
        /* one bar per group colour: the swatch plus one bar, not a zero-value bar as well */
        int c0 = 0, c1 = 0;
        for (const char *p = svg.data; (p = strstr(p, "#ECECFF")); p++) c0++;
        for (const char *p = svg.data; (p = strstr(p, "#8493A6")); p++) c1++;
        if (c0 != 2 || c1 != 2)
            printf("FAIL config overlap: colours %d %d, want 2 2 (bar + swatch, no zero bar)\n", c0, c1), bad++;
        mmd_buffer_free(&svg);
    }
    mmd_writer_reset(w);
    mmd_xy_begin(w, nullptr, false);
    mmd_w_config(w, "xyChart.showDataLabel", "true");
    if (mmd_writer_text(w, &s, &n, &e) != MMD_ERR_SYNTAX) printf("FAIL config after diagram not refused\n"), bad++;
    mmd_writer_reset(w);
    mmd_w_config(w, "xyChart.showDataLabel", "a\"b");
    if (mmd_writer_text(w, &s, &n, &e) != MMD_ERR_SYNTAX) printf("FAIL config quote not refused\n"), bad++;
    mmd_writer_free(w);
    printf("writer config: %d failed\n", bad);
    return bad;
}


int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--dump")) { /* a small corpus: a few labels per slot; [SEED] another one */
        dump_dir = argv[2];
        if (argc > 3) rng ^= strtoull(argv[3], nullptr, 0) * 0x9E3779B97F4A7C15ull | 1;
        int bad = roundtrip(4) + typed(8) + typed2(8);
        printf("writer dump: %u diagrams in %s\n", ndump, dump_dir);
        return bad != 0;
    }
    unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 200;
    int bad = roundtrip(iters) + typed(iters / 4) + typed2(iters / 4) + numbers(iters * 50) + errors() + config_round();
    return bad != 0;
}
