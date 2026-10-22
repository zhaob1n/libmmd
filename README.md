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

## Flowchart features (v2)
- `direction` inside subgraphs: clusters with their own direction are laid out recursively and placed as
  fixed-size proxy nodes; edges to a subgraph id attach to the cluster box; cluster titles are moved
  sideways to avoid edges crossing the title band.
- Crossing reduction: barycenter sweeps + keep-best + budgeted transpose polish (dirty-layer skipping).
- Port ordering for fan-in/fan-out and parallel edges; nested multiple self-loops.
- `mmdc-c --avoid` (`CMM_F_ROUTE_AVOID`): detours polyline segments around nodes they pass through.
- Themes: `default`, `dark`, `forest`, `neutral` via `%%{init: {"theme": ...}}%%`, frontmatter, or `-t`.
- Markdown strings (`"`**bold** *italic*`"`) with auto-wrap at 200px, `fa:fa-name` icons
  (Font Awesome glyphs; needs the font installed to render), edge ids `A e1@--> B`, `e1@{ animate: true }`.
- Golden tests in `tests/golden` (`UPDATE_GOLDEN=1 bash tests/run_tests.sh` to refresh).
- Benchmarks: `bench/results_v2.txt`.

## v3: build options, batch API, routing

- Build: `make` (portable `-O2`), opt-in `make MARCH=native`, `make LTO=1`, `make pgo [LTO=1 MARCH=native]` (output in `build-pgo/`).
- Shared library: `build/libcmermaid.so.0.3.0` (soname `libcmermaid.so.0`, only `cmm_*` exported) plus `build/cmermaid.pc`; `make install PREFIX=/usr DESTDIR=...`.
- Batch rendering: `cmm_context_new()` / `cmm_context_render_svg()` / `cmm_context_free()` reuse the arena and output buffer between calls.
- Obstacle routing is on by default: edges that cross a non-endpoint node get a local visibility-graph (A*) detour. Use `--no-avoid` / `CMM_F_NO_AVOID` for the fastest layout. Edge labels sit on the rendered curve.
- SVG: stroke widths and text anchoring are now CSS classes, and collinear points are dropped (about 20% smaller on medium, about 50% on large vs v1).
- Numbers: `bench/results_v3.txt`.
