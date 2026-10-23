/* Faithful port of dagre's compound ranking: nestingGraph.run + networkSimplex (feasibleTree,
 * cut values, leave/enter edge in graphlib iteration order) + injectEdgeLabelProxies +
 * removeEmptyRanks + nestingGraph.cleanup + normalizeRanks + assignRankMinMax. */
#ifndef MMD_DRANK_H
#define MMD_DRANK_H
#include "base.h"

typedef struct DRankIn {
    size_t N, S;           /* items: real nodes 0..N-1, clusters N..N+S-1 */
    size_t nreal;          /* real nodes taking part, in g.nodes() order */
    const uint32_t *real;  /* [nreal] node index */
    const int64_t *ikey;   /* [N] array-index key (integer-like id) or -1 */
    const Str *id;         /* [N] node ids (JS string order for undirected edge keys) */
    const uint32_t *koff;  /* [S+2] children CSR; parent S is the graph root */
    const uint32_t *kid;   /* items (real v or N+c) in g.children() order */
    size_t E;              /* layout-graph edges in g.edges() order (acyclic, no self loops) */
    const int32_t *eu, *ev, *minlen;
    const uint8_t *lab;    /* edge has a label with width && height */
    int32_t *lrank;        /* optional out [E]: edge-label rank (proxy rank after removeEmptyRanks), -1 if none/fractional */
} DRankIn;

/* rank[N] for the listed real nodes; top/bot[S] = rank of the cluster's _bt/_bb border node
 * (cluster minRank/maxRank), INT32_MAX/-1 for clusters without children. corder[S] lists clusters in
 * border-node creation order (-1 padded). */
[[nodiscard]] bool dagre_rank(Arena *A, const DRankIn *in, int32_t rank[], int32_t top[], int32_t bot[],
                              int32_t corder[]);
#endif
