#ifndef MMD_GRAPH_H
#define MMD_GRAPH_H
#include "base.h"
#include "mmd.h"

typedef enum : uint8_t {
    SH_RECT, SH_ROUND, SH_STADIUM, SH_SUBROUTINE, SH_CYLINDER, SH_CIRCLE, SH_DCIRCLE,
    SH_ASYM, SH_RHOMBUS, SH_HEXAGON, SH_LEAN_R, SH_LEAN_L, SH_TRAP, SH_TRAP_ALT,
    SH_COUNT
} Shape;
typedef enum : uint8_t { DIR_TB, DIR_BT, DIR_LR, DIR_RL } Dir;
typedef enum : uint8_t { ST_NORMAL, ST_DOTTED, ST_THICK, ST_INVISIBLE } Stroke;
typedef enum : uint8_t { AH_NONE, AH_ARROW, AH_CIRCLE, AH_CROSS } Arrow;

constexpr int32_t NONE = -1;
enum { CURVE_BASIS = 0, CURVE_LINEAR, CURVE_STEP, CURVE_STEP_BEFORE, CURVE_STEP_AFTER, CURVE_CARDINAL };
enum { MMD_ACYC_AUTO = 0, MMD_ACYC_DFS = 1, MMD_ACYC_GREEDY = 2 };
enum { MMD_ORDER_AUTO = 0, MMD_ORDER_DAGRE = 1, MMD_ORDER_FAST = 2 };

typedef struct ClassDef { Str name, css; } ClassDef;
typedef struct LinkStyle { int32_t idx; Str css; } LinkStyle; /* idx -1 => default */

typedef struct Graph {
    Arena *arena;
    Dir dir;
    Str title;
    uint8_t theme;  /* mmd_theme */
    uint32_t flags; /* MMD_F_* */
    uint8_t acyclicer; /* MMD_ACYC_* */
    uint8_t order_mode; /* MMD_ORDER_* */
    uint8_t curve;     /* CURVE_* (flowchart.curve) */
    bool iso_toggle;   /* mermaid unified renderer (er/class/state): clusters with no external links lay out in the other direction */
    double node_sep, rank_sep, font_size;

    /* nodes, edges, subgraphs are SoA: graph.c grows every array above the layout-output comments
     * together (ncap/ecap/scap), and layout.c loops_expand copies the node and edge ones */
    size_t nn, ncap;
    Str *nid, *nlabel, *nclass, *nstyle;
    Shape *nshape;
    int32_t *nparent; /* subgraph index or NONE */
    uint8_t *nflags;  /* NF_* */
    double *nw, *nh; /* node size: set by the diagram or measured in the layout */
    double *nx, *ny; /* layout output: position */

    /* edges (SoA) */
    size_t ne, ecap;
    int32_t *efrom, *eto;
    Str *elabel;
    Stroke *estroke;
    Arrow *estart, *eend;
    uint16_t *elen;
    Str *eid;        /* user edge id (e1@-->) or empty */
    uint8_t *eflags; /* EF_* */
    /* layout output: polyline per edge */
    uint32_t *eptoff; /* offset into pts, count in eptn */
    uint32_t *eptn;
    double *elx, *ely, *elw, *elh; /* label box centre/size */

    /* subgraphs */
    size_t ns, scap;
    Str *sid, *stitle, *sclass, *sstyle;
    int32_t *sparent;
    Dir *sdir;
    uint8_t *sflags; /* SF_* */
    /* layout output */
    double *sx, *sy, *sw, *sh;
    double *stx; /* cluster title centre x (avoids edges crossing the title) */

    VEC(ClassDef) classdefs;
    VEC(LinkStyle) linkstyles;
    VEC(double) pts; /* x,y pairs */

    /* intern table: node id -> index+1 */
    uint32_t *htab;
    size_t hcap;
} Graph;

enum { NF_DEFINED = 1u, NF_HIDDEN = 2u, NF_SUBREF = 4u, NF_FIXED = 8u /* size preset (cluster proxy) */, NF_PROXY = 16u /* stands in for an isolated cluster */ };
enum { SF_DIRSET = 1u };
enum { EF_ANIM = 1u, EF_ANIM_SLOW = 2u, EF_LOOPPTS = 4u /* self-loop routed as a polyline (mermaid helpers) */ };

/* ---- tables (graph.c) ---- */
/* make room for one more edge / subgraph; false on OOM or overflow */
[[nodiscard]] bool graph_edge_room(Graph g[static 1]);
/* capacity for exactly nn nodes, ne edges, ns subgraphs in an empty graph (counts are the caller's) */
[[nodiscard]] bool graph_reserve(Graph g[static 1], size_t nn, size_t ne, size_t ns);
[[nodiscard]] bool graph_sg_room(Graph g[static 1]);
/* node index of id, or NONE */
int32_t graph_find(const Graph g[static 1], Str id);
/* node index of id, added (rect, label = id, no parent, sizes 0) if new: *fresh says so; NONE on OOM */
int32_t graph_intern(Graph g[static 1], Str id, bool *fresh);

/* TB/TD/v, BT/^, LR/>, RL/<: false (d untouched) for anything else, so callers keep their default */
[[nodiscard]] bool graph_dir(Str w, Dir d[static 1]);

/* parse flowchart body starting at offset `off` within src */
mmd_status flow_parse(Graph g[static 1], size_t n, const char src[static n], size_t off,
                                    size_t err_off[static 1], char msg[static 160]);
mmd_status flow_layout(Graph g[static 1]);
mmd_status flow_svg(const Graph g[restrict static 1], Buf out[restrict static 1]);
/* mermaid flowchart.curve name -> CURVE_* (unknown => basis) */
uint8_t flow_curve_kind(Str name);
/* self-loop k (0-based, nested outward) on the right side of a node: cubic with both controls at +r,
 * so it bulges SELFLOOP_BULGE*r beyond the node side */
static inline double selfloop_r(unsigned k) { return 40 + 18 * (double)k; }
static inline double selfloop_u(unsigned k) { return 20 + 8 * (double)k; }
constexpr double SELFLOOP_BULGE = 0.75;
constexpr double SELFLOOP_GAP = 6;
void node_size(Graph g[static 1], size_t v);

typedef struct BBox { double x0, y0, x1, y1; } BBox;
static inline void bb_add(BBox bb[static 1], double x0, double y0, double x1, double y1) {
    if (x0 < bb->x0) bb->x0 = x0;
    if (y0 < bb->y0) bb->y0 = y0;
    if (x1 > bb->x1) bb->x1 = x1;
    if (y1 > bb->y1) bb->y1 = y1;
}
/* d3 curveBasis through n points */
void dg_basis(Buf b[restrict static 1], size_t n, const double P[restrict static 2 * n]);

#endif
