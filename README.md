# cmermaid — fast Mermaid renderer in modern C (phase 1)

C23/C2y (`-std=gnu2y` on clang 23, `gnu23` on gcc 14, auto-detected), warning-free with
`-Wall -Wextra -fms-extensions` on both compilers. No dependencies beyond libc + libm.

```
make            # clang (default) -> build/{libcmermaid.a,mmdc-c,bench}
make both       # clang -> build/, gcc -> build-gcc/
make test       # golden/sanity tests (XML well-formedness, error line:col)
make asan       # ASan+UBSan build in build-asan/ ; python3 tests/fuzz.py build-asan/mmdc-c 3000
build/mmdc-c in.mmd -o out.svg      # or: cat in.mmd | build/mmdc-c > out.svg
python3 bench/gen.py && build/bench bench/inputs/large.mmd 30
python3 bench/compare.py bench/mmdr_fixtures/*.mmd   # vs mmdr (MMDR=..., MMDR_ARGS=--fastText)
```

API (`include/cmermaid.h`): `cmm_render_svg(n, src, opts, &buf, &err)`, `cmm_buffer_free`,
`cmm_status_str`; errors carry `code`, `line`, `col`, `message`.

## Pipeline
| stage | file | notes |
|---|---|---|
| arena + views | `src/base.[ch]` | chunked bump arena, in-place growth of last alloc, zero-copy `Str` views, typed `VEC` |
| lexer/parser | `src/flow_parse.c` | single pass, table-driven char classes, open-addressing intern table |
| ranking | `src/nsimplex.c` | greedy FAS (Eades) cycle removal; network simplex (dagre formulation) with graphviz-style incremental cut values / `dfs_range` |
| ordering | `src/layout.c` | DFS init order, up/down barycenter sweeps (≤24, stop after 4 non-improving), Fenwick-tree crossing count, cluster-contiguous keys + dagre-style border chains |
| x coords | `src/layout.c` | Brandes–Köpf: type-1 conflicts, 4 alignments, compaction, balance |
| output | `src/flow_svg.c` | d3 `curveBasis` paths, boundary clipping per shape, markers, styles; fixed-point number formatting (no printf) |
| sequence | `src/seq.c` | participants/actors, all arrows, activations, notes, blocks, autonumber 
| diagram registry | `src/d_registry.c`, `src/diagrams.h` | keyword → renderer table; new types live in `src/d_*.c` |
| shared helpers | `src/dgraph.[ch]` | graph building on top of `flow_layout`, line scanner, SVG primitives |
| stateDiagram(-v2) | `src/d_state.c` | composites (recursive per-scope layout), `--` regions, `[*]`, fork/join/choice, notes, classDef |
| classDiagram | `src/d_class.c` | members, `~generics~`, visibility/`$`/`*`, annotations, all relations + lollipop, cardinality, namespaces, notes |
| erDiagram | `src/d_er.c` | attribute tables (type/name/keys/comment), aliases, all crow's-foot cardinalities (symbolic + word forms) |
| pie | `src/d_pie.c` | slices, showData, title, legend |
| gantt | `src/d_gantt.c` | dayjs-style dateFormat, strftime axisFormat, tickInterval, excludes (weekends/dates), after/until deps, done/active/crit/milestone/vert |
| journey | `src/d_journey.c` | sections, scored tasks with faces, actor legend/dots |
| mindmap | `src/d_mindmap.c` | indentation tree, all node shapes, two-sided tidy layout, section colours |
| timeline | `src/d_timeline.c` | sections, periods, multi-event / continuation lines, wrapping |
| quadrantChart | `src/d_quadrant.c` | axes, quadrant labels, points with inline style and classDef |
| xychart(-beta) | `src/d_xychart.c` | vertical/horizontal, categories or numeric range, bar + line series, nice ticks |
| gitGraph | `src/d_gitgraph.c` | LR/TB/BT, branch order, merge, cherry-pick, tags, NORMAL/REVERSE/HIGHLIGHT |
| text metrics | `src/text.c` | embedded Helvetica-compatible advance table, CJK/emoji wide, 8-byte ASCII fast path |
