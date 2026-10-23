#include <stdio.h>

#include "diagrams.h"

mmd_status diag_fail(size_t *err_off, char msg[static 160], size_t off, const char *m) {
    *err_off = off;
    snprintf(msg, 160, "%s", m);
    return MMD_ERR_SYNTAX;
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
    {S("architecture"), architecture_render},
    {S("architecture-beta"), architecture_render},
    {S("C4Deployment"), c4_render},
    {S("C4Dynamic"), c4_render},
    {S("C4Component"), c4_render},
    {S("C4Container"), c4_render},
    {S("C4Context"), c4_render},
    {S("requirementDiagram"), requirement_render},
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
    {S("zenuml"), zenuml_render},
    {S("ishikawa-beta"), ishikawa_render},
    {S("ishikawa"), ishikawa_render},
    {S("cynefin-beta"), cynefin_render},
    {S("cynefin"), cynefin_render},
    {S("venn-beta"), venn_render},
    {S("venn"), venn_render},
    {S("treeView-beta"), treeview_render},
    {S("treeView"), treeview_render},
    {S("eventmodeling"), eventmodeling_render},
    {S("railroad-beta"), railroad_render},
    {S("railroad-ebnf-beta"), railroad_render},
    {S("railroad-abnf-beta"), railroad_render},
    {S("railroad-peg-beta"), railroad_render},
    {S("wardley-beta"), wardley_render},
};

DiagFn diag_lookup(Str kw) {
    for (size_t i = 0; i < countof(DIAGS); i++)
        if (str_eq(kw, DIAGS[i].kw)) return DIAGS[i].fn;
    return nullptr;
}
