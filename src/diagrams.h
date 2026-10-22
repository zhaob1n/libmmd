/* Entry points of the non-flowchart diagram types (one translation unit each, src/d_*.c). */
#ifndef CMM_DIAGRAMS_H
#define CMM_DIAGRAMS_H
#include "base.h"
#include "cmermaid.h"

#define DIAG_FN(name)                                                                              \
    cmm_status name(Arena *a, size_t n, const char src[static n], size_t off, Str title, double fs, \
                    Buf *out, size_t *err_off, char msg[static 160])
typedef DIAG_FN((*DiagFn));

[[nodiscard]] DIAG_FN(state_render);
[[nodiscard]] DIAG_FN(class_render);
[[nodiscard]] DIAG_FN(er_render);
[[nodiscard]] DIAG_FN(pie_render);
[[nodiscard]] DIAG_FN(gantt_render);
[[nodiscard]] DIAG_FN(journey_render);
[[nodiscard]] DIAG_FN(mindmap_render);
[[nodiscard]] DIAG_FN(timeline_render);
[[nodiscard]] DIAG_FN(quadrant_render);
[[nodiscard]] DIAG_FN(xychart_render);
[[nodiscard]] DIAG_FN(gitgraph_render);
[[nodiscard]] DIAG_FN(architecture_render);
[[nodiscard]] DIAG_FN(c4_render);
[[nodiscard]] DIAG_FN(requirement_render);
[[nodiscard]] DIAG_FN(block_render);
[[nodiscard]] DIAG_FN(sankey_render);
[[nodiscard]] DIAG_FN(kanban_render);
[[nodiscard]] DIAG_FN(treemap_render);
[[nodiscard]] DIAG_FN(radar_render);
[[nodiscard]] DIAG_FN(packet_render);
[[nodiscard]] DIAG_FN(zenuml_render);
[[nodiscard]] DIAG_FN(ishikawa_render);
[[nodiscard]] DIAG_FN(cynefin_render);

/* keyword -> renderer, nullptr when not a diagram type handled here */
[[nodiscard]] DiagFn diag_lookup(Str kw);

/* shared error helper */
[[nodiscard]] cmm_status diag_fail(size_t *err_off, char msg[static 160], size_t off, const char *m);
#endif
