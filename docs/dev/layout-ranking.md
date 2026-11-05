# Layout: why there are two rankers and two orderers

`layout_core` (src/layout.c) picks one of two pipelines per graph:

| path | when | rank | order |
|------|------|------|-------|
| dagre-faithful | `order_mode != FAST`, `S <= DAGRE_ORDER_MAX_S`, and (`order_mode == DAGRE` or `N <= DAGRE_RANK_MAX_N`) | `dagre_rank` (src/drank.c) | `dagre_order` (src/dorder.c) |
| fast | everything else | `network_simplex` (src/nsimplex.c), `greedy_fas` for big cyclic graphs | barycenter sweeps in layout.c |

They are deliberately not merged:

- **drank.c** ports dagre's nesting graph (`_root`/`_bt`/`_bb` border nodes),
  makeSpaceForEdgeLabels and networkSimplex *including graphlib's iteration
  orders*: integer-like ids first, then insertion order, and undirected edge
  keys in JS string order (hence the generated `_bt1000123` names and the name
  sort). Ties between equally optimal rankings are broken exactly the way
  mermaid does, which is what keeps the output byte-identical to mermaid for
  small and medium diagrams.
- **nsimplex.c** is the same algorithm (Gansner et al.) over plain CSR arrays
  without any of that bookkeeping. It is much faster on big graphs but picks
  different optimal rankings, so swapping one for the other changes output.

Unifying them would mean either slowing large graphs down to dagre's
bookkeeping or giving up mermaid parity on small ones. Changes to either file
must keep `bench/eq/cmp.sh` at `bad=0`.

dorder.c scratch: small graphs (`NS <= 1024 && S <= 32`) allocate from the
render arena; larger ones use tracked malloc blocks (`Heap`). A per-call Arena
was tried and regressed about 20% on cluster-heavy graphs, because its big
blocks are mmapped and page-faulted fresh on every call.
