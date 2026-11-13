/* Entry points of the non-flowchart diagram types (one translation unit each, src/d_*.c). */
#ifndef MMD_DIAGRAMS_H
#define MMD_DIAGRAMS_H
#include "base.h"
#include "mmd.h"

#define DIAG_FN(name)                                                                                    \
    mmd_status name(Arena a[restrict static 1], size_t n, const char src[restrict static n], size_t off, \
                    [[maybe_unused]] Str title, [[maybe_unused]] double fs,                              \
                    Buf out[restrict static 1], size_t err_off[restrict static 1], char msg[restrict static 160])
typedef DIAG_FN((*DiagFn));

DIAG_FN(state_render);
DIAG_FN(class_render);
DIAG_FN(er_render);
DIAG_FN(pie_render);
DIAG_FN(gantt_render);
DIAG_FN(journey_render);
DIAG_FN(mindmap_render);
DIAG_FN(timeline_render);
DIAG_FN(quadrant_render);
DIAG_FN(xychart_render);
DIAG_FN(gitgraph_render);
DIAG_FN(architecture_render);
DIAG_FN(c4_render);
DIAG_FN(requirement_render);
DIAG_FN(block_render);
DIAG_FN(sankey_render);
DIAG_FN(kanban_render);
DIAG_FN(treemap_render);
DIAG_FN(usecase_render);
DIAG_FN(radar_render);
DIAG_FN(packet_render);
DIAG_FN(zenuml_render);
DIAG_FN(ishikawa_render);
DIAG_FN(cynefin_render);
DIAG_FN(venn_render);
DIAG_FN(treeview_render);
DIAG_FN(eventmodeling_render);
DIAG_FN(railroad_render);
DIAG_FN(wardley_render);
/* swimlane-beta: flowchart parse/draw with a lane layout (d_swimlanes.c) */
struct Graph;
mmd_status swim_layout(struct Graph *g, Str *keep[static 1]);
mmd_status swim_titles(const struct Graph *g, const Str *keep, Buf b[static 1]);

/* agentflow-beta: flowchart parse/layout, collapsed containers folded and the neo look drawn (d_agentflow.c) */
typedef struct AfInfo AfInfo;
mmd_status af_prepare(struct Graph *g, AfInfo *out[static 1]);
bool af_neo(const AfInfo *inf);
mmd_status af_svg(const struct Graph *g, const AfInfo *inf, Buf *b);

/* keyword -> renderer (nullptr: drawn in mmd.c, or no diagram type), the section of mermaid's
 * config that holds its settings (getDiagramConfigKey), and the key mermaid's detectInit files a
 * directive's `config` under: the detector id, which no renderer reads for class, state, xychart,
 * flowchart-elk and the railroad dialects. Both are empty for an unknown keyword. */
typedef struct Diag { DiagFn fn; Str sect, init; } Diag;
Diag diag_lookup(Str kw);

/* shared error helper */
mmd_status diag_fail(size_t err_off[static 1], char msg[static 160], size_t off, const char m[static 1]);
#endif
