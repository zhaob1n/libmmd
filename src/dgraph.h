/* Shared helpers for diagram types that reuse the layered layout (state, class, er, mindmap...).
 * Builds a Graph directly (no flowchart parser), lets callers pre-size nodes (NF_FIXED),
 * and provides small SVG primitives. Owned by the diagram-types work; flow_* files untouched. */
#ifndef CMM_DGRAPH_H
#define CMM_DGRAPH_H
#include "graph.h"

void dg_init(Graph *g, Arena *A, Dir dir, double fs);
/* intern node by id; NONE on OOM. *fresh set when created now (may be nullptr). */
[[nodiscard]] int32_t dg_node(Graph *g, Str id, bool *fresh);
/* lookup only */
[[nodiscard]] int32_t dg_find(const Graph *g, Str id);
static inline void dg_size(Graph *g, int32_t v, double w, double h) {
    g->nw[v] = w;
    g->nh[v] = h;
    g->nflags[v] |= NF_FIXED;
}
[[nodiscard]] int32_t dg_edge(Graph *g, int32_t a, int32_t b, Str label);
[[nodiscard]] int32_t dg_cluster(Graph *g, Str id, Str title, int32_t parent);

/* ---- SVG ---- */
typedef struct BBox { double x0, y0, x1, y1; } BBox;
static inline void bb_add(BBox *bb, double x0, double y0, double x1, double y1) {
    if (x0 < bb->x0) bb->x0 = x0;
    if (y0 < bb->y0) bb->y0 = y0;
    if (x1 > bb->x1) bb->x1 = x1;
    if (y1 > bb->y1) bb->y1 = y1;
}
constexpr BBox BB_EMPTY = {.x0 = 1e300, .y0 = 1e300, .x1 = -1e300, .y1 = -1e300};
/* bbox of laid-out nodes, edge points, edge labels and clusters */
[[nodiscard]] BBox dg_bbox(const Graph *g);
void dg_pt(Buf *b, double x, double y);
/* d3 curveBasis through n points */
void dg_basis(Buf *b, size_t n, const double P[static 2 * n]);
/* path data for edge e (basis curve, or a loop for self edges) */
void dg_edge_d(Buf *b, const Graph *g, size_t e);
/* centred multi-line text; anchor "middle"/"start"/"end"; cls may be nullptr */
void dg_text(Buf *b, Str s, double fs, double x, double cy, const char *anchor, const char *cls);
/* svg root with viewBox, <style>, white background */
void dg_head(Buf *b, BBox bb, double margin, const char *kind, size_t ncss, const char css[static ncss]);
#define DG_HEAD(b, bb, m, kind, css) dg_head((b), (bb), (m), (kind), sizeof(css) - 1, (css))
void dg_rect(Buf *b, double x, double y, double w, double h, double rx, const char *cls);
void dg_line(Buf *b, double x1, double y1, double x2, double y2, const char *cls);

/* tiny line scanner shared by line-oriented diagram parsers */
typedef struct LineIt { const char *s; size_t n, i, lstart; } LineIt;
/* next non-empty, non-comment trimmed line; *off = offset of its first byte */
bool dg_next_line(LineIt *it, Str *line, size_t *off);
/* indentation (columns, tab = 4) of the raw line starting at offset off */
unsigned dg_indent(const LineIt *it, size_t off);
Str dg_word(Str *s);
bool dg_parse_num(Str s, double *out);
#endif

/* opening <text> tag (caller writes the content and "</text>") */
void dg_text_open(Buf *b, double x, double cy, const char *anchor, const char *cls);
/* rect with explicit fill (nullptr = class only) */
void dg_rect_fill(Buf *b, double x, double y, double w, double h, double rx, const char *cls, const char *fill);
/* palette of mermaid default theme accents, cycling */
[[nodiscard]] const char *dg_palette(size_t i);
/* normalise (<br>, quotes) then greedily word-wrap to maxw; nullptr-safe: returns {} on OOM */
[[nodiscard]] Str dg_wrap(Arena *a, Str raw, double fs, double maxw);
