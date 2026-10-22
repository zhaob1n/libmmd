/* Faithful port of dagre's order() step (lib/order: initOrder, buildLayerGraph, barycenter,
 * sortSubgraph, resolveConflicts, sort, addSubgraphConstraints, crossCount). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "base.h"

/* "dnodes" are layout nodes 0..V-1 plus cluster nodes V..V+S-1. */
typedef struct DOrder {
    size_t V, S, R;
    const int32_t *rank;  /* [V] rank or -1 (absent) */
    const int32_t *par;   /* [V+S] parent cluster index (0..S-1) or -1 for the root */
    size_t nn;
    const uint32_t *nord; /* [nn] dagre g.nodes() order over present dnodes */
    const int64_t *ikey;  /* [V+S] JS array-index value of the id, or -1 */
    const uint32_t *poff, *pidx, *soff, *sidx; /* [V] predecessor / successor lists (with multiplicity) */
    const uint32_t *doff, *didx;               /* [V] initOrder DFS successor order */
    const int32_t *cmin, *cmax;                /* [S] cluster rank span (cmax < cmin: empty) */
    const uint32_t *bbase;                     /* [S] border nodes: bbase + 2*(r-cmin) + {0 left, 1 right} */
    const uint32_t *loff;                      /* [R+1] layer offsets into lay */
    int32_t *lay, *pos;                        /* out */
} DOrder;

/* Returns false on allocation failure. Result: lay/pos hold dagre's best ordering. */
[[nodiscard]] bool dagre_order(Arena *A, const DOrder *d, uint64_t *crossings);
