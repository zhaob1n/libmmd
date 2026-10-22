#include <stdio.h>

#include "diagrams.h"

cmm_status diag_fail(size_t *err_off, char msg[static 160], size_t off, const char *m) {
    *err_off = off;
    snprintf(msg, 160, "%s", m);
    return CMM_ERR_SYNTAX;
}

typedef struct DiagEnt { Str kw; DiagFn fn; } DiagEnt;
static const DiagEnt DIAGS[] = {
    {S("stateDiagram"), state_render},
    {S("stateDiagram-v2"), state_render},
    {S("classDiagram"), class_render},
    {S("classDiagram-v2"), class_render},
    {S("erDiagram"), er_render},
    {S("pie"), pie_render},
    {S("gantt"), gantt_render},
    {S("journey"), journey_render},
    {S("timeline"), timeline_render},
    {S("quadrantChart"), quadrant_render},
    {S("xychart-beta"), xychart_render},
    {S("xychart"), xychart_render},
    {S("gitGraph"), gitgraph_render},
    {S("mindmap"), mindmap_render},
    {S("block-beta"), block_render},
    {S("block"), block_render},
    {S("sankey-beta"), sankey_render},
    {S("sankey"), sankey_render},
    {S("kanban"), kanban_render},
    {S("treemap-beta"), treemap_render},
    {S("treemap"), treemap_render},
    {S("radar-beta"), radar_render},
    {S("radar"), radar_render},
    {S("packet-beta"), packet_render},
    {S("packet"), packet_render},
};

DiagFn diag_lookup(Str kw) {
    for (size_t i = 0; i < countof(DIAGS); i++)
        if (str_eq(kw, DIAGS[i].kw)) return DIAGS[i].fn;
    return nullptr;
}
