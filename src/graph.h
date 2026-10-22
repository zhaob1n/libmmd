#ifndef CMM_GRAPH_H
#define CMM_GRAPH_H
#include "base.h"
#include "cmermaid.h"

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
enum { CMM_ACYC_AUTO = 0, CMM_ACYC_DFS = 1, CMM_ACYC_GREEDY = 2 };

typedef struct ClassDef { Str name, css; } ClassDef;
typedef struct LinkStyle { int32_t idx; Str css; } LinkStyle; /* idx -1 => default */

typedef struct Graph {
    Arena *arena;
    Dir dir;
    Str title;
    uint8_t theme;  /* cmm_theme */
    uint32_t flags; /* CMM_F_* */
    uint8_t acyclicer; /* CMM_ACYC_* */
    uint8_t curve;     /* CURVE_* (flowchart.curve) */
    double node_sep, rank_sep, font_size;

    /* nodes (SoA) */
    size_t nn, ncap;
    Str *nid, *nlabel, *nclass, *nstyle;
    Shape *nshape;
    int32_t *nparent; /* subgraph index or NONE */
    uint8_t *nflags;  /* NF_* */
    double *nw, *nh, *nx, *ny;

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
    double *sx, *sy, *sw, *sh;
    double *stx; /* cluster title centre x (avoids edges crossing the title) */

    VEC(ClassDef) classdefs;
    VEC(LinkStyle) linkstyles;
    VEC(double) pts; /* x,y pairs */

    /* intern table: node id -> index+1 */
    uint32_t *htab;
    size_t hcap;
} Graph;

enum { NF_DEFINED = 1u, NF_HIDDEN = 2u, NF_SUBREF = 4u, NF_FIXED = 8u /* size preset (cluster proxy) */ };
enum { SF_DIRSET = 1u };
enum { EF_ANIM = 1u, EF_ANIM_SLOW = 2u };

/* parse flowchart body starting at offset `off` within src */
[[nodiscard]] cmm_status flow_parse(Graph *g, size_t n, const char src[static n], size_t off,
                                    size_t *err_off, char msg[static 160]);
[[nodiscard]] cmm_status flow_layout(Graph *g);
[[nodiscard]] cmm_status flow_svg(const Graph *g, Buf *out);
uint8_t flow_curve_kind(Str name); /* mermaid flowchart.curve name -> CURVE_* (unknown => basis) */
void node_size(Graph *g, size_t v);

#endif
