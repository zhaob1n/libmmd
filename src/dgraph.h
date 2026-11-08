/* Shared helpers for diagram types that reuse the layered layout (state, class, er, mindmap...).
 * Builds a Graph directly (no flowchart parser), lets callers pre-size nodes (NF_FIXED),
 * and provides small SVG primitives. Owned by the diagram-types work; flow_* files untouched. */
#ifndef MMD_DGRAPH_H
#define MMD_DGRAPH_H
#include "graph.h"

void dg_init(Graph g[static 1], Arena A[static 1], Dir dir, double fs);
/* intern node by id; NONE on OOM. *fresh set when created now (may be nullptr). */
[[nodiscard]] int32_t dg_node(Graph g[static 1], Str id, bool *fresh);
/* lookup only */
[[nodiscard]] int32_t dg_find(const Graph g[static 1], Str id);
static inline void dg_size(Graph g[static 1], int32_t v, double w, double h) {
    g->nw[v] = w;
    g->nh[v] = h;
    g->nflags[v] |= NF_FIXED;
}
[[nodiscard]] int32_t dg_edge(Graph g[static 1], int32_t a, int32_t b, Str label);
[[nodiscard]] int32_t dg_cluster(Graph g[static 1], Str id, Str title, int32_t parent);

/* ---- SVG ---- */
constexpr BBox BB_EMPTY = {.x0 = 1e300, .y0 = 1e300, .x1 = -1e300, .y1 = -1e300};
/* bbox of laid-out nodes, edge points, edge labels and clusters */
[[nodiscard]] BBox dg_bbox(const Graph g[static 1]);
/* filled 10-long arrowhead at tip, pointing away from from; half = half the base width */
void dg_arrowhead(Buf b[restrict static 1], const char cls[restrict static 1], const double tip[restrict static 2],
                  const double from[restrict static 2], double half);
/* path data for edge e (basis curve, or a loop for self edges) */
void dg_edge_d(Buf b[restrict static 1], const Graph g[restrict static 1], size_t e);
/* centred multi-line text; anchor "middle"/"start"/"end"; cls may be nullptr */
void dg_text(Buf b[restrict static 1], Str s, double fs, double x, double cy, const char anchor[restrict static 1],
             const char *restrict cls);
/* dg_text with an explicit row pitch (font size comes from the class) */
void dg_text_lh(Buf b[restrict static 1], Str s, double lh, double x, double cy, const char anchor[restrict static 1],
                const char *restrict cls);
/* svg root with viewBox, <style>, white background */
void dg_head(Buf b[restrict static 1], BBox bb, double margin, const char kind[restrict static 1], size_t ncss,
             const char css[restrict static ncss]);
#define DG_HEAD(b, bb, m, kind, css) dg_head((b), (bb), (m), (kind), sizeof(css) - 1, (css))
void dg_rect(Buf b[restrict static 1], double x, double y, double w, double h, double rx, const char *restrict cls);
void dg_line(Buf b[restrict static 1], double x1, double y1, double x2, double y2, const char *restrict cls);

/* tiny line scanner shared by line-oriented diagram parsers */
typedef struct LineIt { const char *s; size_t n, i, lstart; } LineIt;
/* next non-empty, non-comment trimmed line; *off = offset of its first byte */
[[nodiscard]] bool dg_next_line(LineIt it[static 1], Str line[static 1], size_t off[static 1]);
/* indentation (columns, tab = 4) of the raw line starting at offset off */
[[nodiscard]] unsigned dg_indent(const LineIt it[static 1], size_t off);
[[nodiscard]] Str dg_word(Str s[static 1]);
/* trim, then drop one pair of matching outer quotes: '"' always, '\'' too when apos */
static inline Str dg_unquote(Str s, bool apos) {
    s = str_trim(s);
    if (s.n >= 2 && (s.p[0] == '"' || (apos && s.p[0] == '\'')) && s.p[s.n - 1] == s.p[0]) s = (Str){.p = s.p + 1, .n = s.n - 2};
    return s;
}
/* user-supplied CSS value safe to splice into a style attribute */
static inline bool dg_css_ok(Str v) {
    if (!v.n || v.n > 40) return false;
    for (size_t i = 0; i < v.n; i++) if (strchr(";{}<>\"'\\", v.p[i])) return false;
    return true;
}
[[nodiscard]] bool dg_parse_num(Str s, double out[static 1]);
/* ---- free-form (non-graph) renderers: draw into a body Buf while growing a BBox ---- */
typedef enum TBase { TB_ALPHA, TB_MIDDLE, TB_HANG } TBase;
/* mermaid drawMultilineText: lines ('\n' or <br>) as tspans, first at y (block centred when
 * centre is true); grows *bb (nullable) with an estimated text box. Returns the line count. */
unsigned dg_tspans(Buf b[restrict static 1], BBox *restrict bb, Str s, double fs, double lh, double x, double y, bool centre,
                   const char anchor[restrict static 1], TBase base, const char *restrict cls);
/* estimated bbox of such a text block (same arguments) */
[[nodiscard]] BBox dg_tbox(Str s, double fs, double lh, double x, double y, bool centre, const char anchor[static 1], TBase base);
/* greedy word wrap at maxc characters per line (mermaid wrapText), '\n'-joined; {} on OOM */
[[nodiscard]] Str dg_wrapc(Arena a[static 1], Str s, size_t maxc);
/* svg head for bb + margin, then body, then </svg>; frees body */
[[nodiscard]] mmd_status dg_finish(Buf out[restrict static 1], Buf body[restrict static 1], BBox bb, double m,
                                   const char kind[restrict static 1], size_t ncss, const char css[restrict static ncss]);
#define DG_FINISH(o, body, bb, m, kind, css) dg_finish((o), (body), (bb), (m), (kind), sizeof(css) - 1, (css))
#endif

/* opening <text> tag (caller writes the content and "</text>") */
void dg_text_open(Buf b[restrict static 1], double x, double cy, const char anchor[restrict static 1], const char *restrict cls);
/* rect with explicit fill (nullptr = class only) */
void dg_rect_fill(Buf b[restrict static 1], double x, double y, double w, double h, double rx, const char *restrict cls,
                  const char *restrict fill);
/* palette of mermaid default theme accents, cycling */
[[nodiscard]] const char *dg_palette(size_t i);
/* normalise (<br>, quotes) then greedily word-wrap to maxw; nullptr-safe: returns {} on OOM */
[[nodiscard]] Str dg_wrap(Arena a[static 1], Str raw, double fs, double maxw);
[[nodiscard]] Str dg_wrapf(Arena a[static 1], Str raw, double fs, double maxw, double (*meas)(Str, double));
