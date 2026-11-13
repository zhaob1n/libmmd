#include <stdio.h>

#include "diagrams.h"

mmd_status diag_fail(size_t err_off[static 1], char msg[static 160], size_t off, const char m[static 1]) {
    *err_off = off;
    snprintf(msg, 160, "%s", m);
    return MMD_ERR_SYNTAX;
}

typedef struct DiagEnt { Str kw; Diag d; } DiagEnt;
/* D: the detector id is the config section; DI: it is not (init) */
#define D(kw, fn, sect) {S(kw), {fn, S(sect), S(sect)}}
#define DI(kw, fn, sect, init) {S(kw), {fn, S(sect), S(init)}}
static const DiagEnt DIAGS[] = {
    D("graph", nullptr, "flowchart"),
    D("flowchart", nullptr, "flowchart"),
    DI("flowchart-elk", nullptr, "flowchart", "flowchart-elk"),
    D("swimlane-beta", nullptr, "swimlane"),
    D("agentflow-beta", nullptr, "agentflow"),
    D("sequenceDiagram", nullptr, "sequence"),
    DI("stateDiagram", state_render, "state", "stateDiagram"),
    DI("stateDiagram-v2", state_render, "state", "stateDiagram"),
    DI("classDiagram", class_render, "class", "classDiagram"),
    DI("classDiagram-v2", class_render, "class", "classDiagram"),
    D("erDiagram", er_render, "er"),
    D("pie", pie_render, "pie"),
    D("gantt", gantt_render, "gantt"),
    D("journey", journey_render, "journey"),
    D("timeline", timeline_render, "timeline"),
    D("quadrantChart", quadrant_render, "quadrantChart"),
    DI("xychart-beta", xychart_render, "xyChart", "xychart"),
    DI("xychart", xychart_render, "xyChart", "xychart"),
    D("gitGraph", gitgraph_render, "gitGraph"),
    D("mindmap", mindmap_render, "mindmap"),
    D("architecture", architecture_render, "architecture"),
    D("architecture-beta", architecture_render, "architecture"),
    D("C4Deployment", c4_render, "c4"),
    D("C4Dynamic", c4_render, "c4"),
    D("C4Component", c4_render, "c4"),
    D("C4Container", c4_render, "c4"),
    D("C4Context", c4_render, "c4"),
    D("requirementDiagram", requirement_render, "requirement"),
    D("block-beta", block_render, "block"),
    D("block", block_render, "block"),
    D("sankey-beta", sankey_render, "sankey"),
    D("sankey", sankey_render, "sankey"),
    D("kanban", kanban_render, "kanban"),
    D("treemap-beta", treemap_render, "treemap"),
    D("treemap", treemap_render, "treemap"),
    D("radar-beta", radar_render, "radar"),
    D("radar", radar_render, "radar"),
    D("packet-beta", packet_render, "packet"),
    D("packet", packet_render, "packet"),
    D("zenuml", zenuml_render, "zenuml"),
    D("ishikawa-beta", ishikawa_render, "ishikawa"),
    D("ishikawa", ishikawa_render, "ishikawa"),
    D("cynefin-beta", cynefin_render, "cynefin"),
    D("cynefin", cynefin_render, "cynefin"),
    D("venn-beta", venn_render, "venn"),
    D("venn", venn_render, "venn"),
    D("treeView-beta", treeview_render, "treeView"),
    D("treeView", treeview_render, "treeView"),
    D("eventmodeling", eventmodeling_render, "eventmodeling"),
    D("railroad-beta", railroad_render, "railroad"),
    DI("railroad-ebnf-beta", railroad_render, "railroad", "railroadEbnf"),
    DI("railroad-abnf-beta", railroad_render, "railroad", "railroadAbnf"),
    DI("railroad-peg-beta", railroad_render, "railroad", "railroadPeg"),
    D("wardley-beta", wardley_render, "wardley"),
    D("usecase-beta", usecase_render, "usecase"),
};
#undef D
#undef DI

Diag diag_lookup(Str kw) {
    for (size_t i = 0; i < countof(DIAGS); i++)
        if (str_eq(kw, DIAGS[i].kw)) return DIAGS[i].d;
    return (Diag){};
}
