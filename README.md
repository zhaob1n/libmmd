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
| sequence | `src/seq.c` | participants/actors, all arrows, activations, notes, blocks, autonumber, `A->>()B` / `A()->>B` central-connection dots |
| diagram registry | `src/d_registry.c`, `src/diagrams.h` | keyword → renderer table; new types live in `src/d_*.c` |
| shared helpers | `src/dgraph.[ch]` | graph building on top of `flow_layout`, line scanner, SVG primitives |
| stateDiagram(-v2) | `src/d_state.c` | composites (recursive per-scope layout), `--` regions, `[*]`, fork/join/choice, notes, classDef |
| classDiagram | `src/d_class.c` | members, `~generics~`, visibility/`$`/`*`, annotations, all relations + lollipop, cardinality, namespaces, notes |
| erDiagram | `src/d_er.c` | attribute tables (type/name/keys/comment), aliases, all crow's-foot cardinalities (symbolic + word forms), `:::cls` / classDef / class / style, `subgraph` clusters (relations may target a subgraph id) |
| pie | `src/d_pie.c` | slices, showData, title, legend |
| gantt | `src/d_gantt.c` | dayjs-style dateFormat (incl. Z/ZZ offsets), todayMarker (CMM_TODAY override), weekday/weekend friday, strftime axisFormat, tickInterval, excludes (weekends/dates), after/until deps, done/active/crit/milestone/vert, invalid tickInterval ignored, `gantt.useWidth` |
| journey | `src/d_journey.c` | sections, scored tasks with faces, actor legend/dots |
| mindmap | `src/d_mindmap.c` | indentation tree, all node shapes, radial layout (tidy-tree opt-in), `::icon(fa ...)` drawn as real Font Awesome paths (badge fallback), `:::class`, section colours |
| timeline | `src/d_timeline.c` | sections, periods, multi-event / continuation lines, wrapping |
| quadrantChart | `src/d_quadrant.c` | axes, quadrant labels, points with inline style and classDef |
| xychart(-beta) | `src/d_xychart.c` | vertical/horizontal, categories or numeric range, bar + line series, nice ticks, legend for titled series |
| gitGraph | `src/d_gitgraph.c` | LR/TB/BT, branch order, merge, cherry-pick, tags, NORMAL/REVERSE/HIGHLIGHT, lane-routed merges, mermaid lane order (unordered branches = `0.<index>`), hashed auto ids |
| sankey(-beta) | `src/d_sankey.c` | RFC-4180 CSV, longest-path columns (sinks justified), barycentric relaxation, value labels with collision avoidance |
| block(-beta) | `src/d_block.c` | columns, spans, `space`, nested blocks, 10 shapes, labelled edges clipped to the true shape outline; grid layout |
| requirementDiagram | `src/d_requirement.c` | 6 requirement kinds + element, all 7 relations (grey label plates), `direction`; layered layout |
| C4* | `src/d_c4.c` | Context/Container/Component/Dynamic/Deployment, Person/System/Container/Component(+Db/Queue/_Ext), boundaries, Rel variants, `c4ShapeInRow`/`c4BoundaryInRow` grid, UpdateLayoutConfig/UpdateElementStyle/UpdateRelStyle (colours whitelisted), mermaid-11 person shape + `[Type: techn]` line |
| architecture(-beta) | `src/d_architecture.c` | groups (nested), services, junctions, port-constraint grid, orthogonal edges, non-overlapping groups, built-in icons + embedded `logos:`/`fa:` subset (src/icons_data.h via tools/genicons.js: simple-icons CC0, FA Free CC-BY-4.0) |
| packet(-beta) | `src/d_packet.c` | bit ranges, `+N`, row splitting, bit numbers, title below (as mermaid) |
| radar(-beta) | `src/d_radar.c` | axes, positional/named curves, max/min/ticks, polygon graticule, legend |
| treemap(-beta) | `src/d_treemap.c` | squarified layout, sections, classDef/`:::` |
| kanban | `src/d_kanban.c` | columns, cards, `@{assigned, ticket, priority}`; named themes colour columns like mermaid (cScale[n+2] lightened/darkened 10%, cScaleLabel titles) |
| ishikawa | `src/d_ishikawa.c` | fishbone port of mermaid's renderer: alternating up/down causes, nested sub-causes along bones, wrapped head |
| cynefin | `src/d_cynefin.c` | five domains, items (confusion capped at 3 + overflow badge), transitions; seeded wavy boundaries match mermaid-cli byte-for-byte |
| ZenUML | `src/d_zenuml.c` | native renderer in ZenUML's look: titled outer frame, boxed participant headers with `@Actor/@Database/@Boundary/@Control/@Entity/@Collections/@Queue` icons, unnamed starter column (or `@Starter(X)`), hierarchical message numbers (1, 2.1, 2.1.1), `// comments` above the next message, sync calls with `{}` bodies (activations, explicit `return` plus `x =` assignment return), `new X()` creating an `x:X` box mid-diagram, async `A->B: msg` / dashed `A-->B`, Alt/Loop/Opt/Par/Try/Critical/Section fragments (if/else if/else, while/for/forEach/loop, try/catch/finally) with numbered headers, ZenUML-style header icons and condition rows, title |
| config / a11y | `src/d_cfg.c`, `src/dcfg.h` | `%%{init}%%` (relaxed JSON) + YAML frontmatter `config:` flattened to dotted keys; ~85 themeVariables mapped to CSS per type (values whitelisted, `url(` rejected); named themes `dark`/`forest`/`neutral` for every non-flowchart type (presets generated by `tools/gen_themes.mjs` from mermaid's own theme objects into `src/d_themes.h`, incl. pie1..12/cScale/git palettes; user themeVariables override); per-diagram keys (pie.textPosition, timeline.disableMulticolor, sankey.showValues, quadrantChart.chartHeight, gantt.axisFormat/tickInterval, gantt.barHeight/barGap/topPadding/leftPadding, quadrantChart.chartWidth, xyChart.width/height, sankey.width/height, packet.bitsPerRow); accTitle/accDescr (incl. `{}` block) → `<title>`/`<desc>` + aria-labelledby/describedby |
| text metrics | `src/text.c` | embedded Helvetica-compatible advance table, CJK/emoji wide, 8-byte ASCII fast path |
| venn | `src/d_venn.c` | sets, unions with sizes, indented `text` nodes, `style`; venn.js-style area-proportional layout (lens-area bisection + stress descent), labels at each region's deepest point |
| treeView | `src/d_treeview.c` | indentation or box-drawing (`├──`/`└──`/`│`) trees, `:::highlight`, `icon(pack:name)` / showIcons file+folder icons, filename/extension icon maps, `## descriptions` |

## Docs syntax corpus

`tests/docs/<doc>/NN.mmd`: all 432 ```` ```mermaid ```` examples from mermaid's `packages/mermaid/src/docs/syntax/*.md` (mermaid 6d8921f, extracted by `tools/extract_docs.py`). `python3 tools/docs_audit.py [-v] [--mmdc]` reports per-type pass counts against mermaid-cli (cached in `tests/docs/mermaid_status.txt`); `tools/docs_sbs.py` renders side-by-sides. Every supported type passes 100% of the examples mermaid itself renders (312/396 total). The 84 failures are all types we do not implement: cynefin, eventmodeling, ishikawa, railroad, swimlanes, treeView, venn, wardley.

## Fidelity vs mermaid-cli 11.17

Side-by-side renders (left: cmermaid, right: mermaid 11.17) live in `samples/sbs/*.png`.
Already fixed:
- C4: grid layout, person shape, centred boundary titles, relation labels placed to avoid boxes and other labels;
- gitGraph: lane order, vivid palette, commit labels rotated -45 degrees;
- treemap and radar: cScale palettes;
- gantt: excluded days/weekends shaded, light grid;
- kanban: mermaid column/card widths;
- mindmap: compacted with a spring relaxation;
- pie: declaration order and palette;
- timeline: cScale section colours;
- sequence: participant `@{type}` shapes;
- packet: title position;
- requirement: label plates.

Known remaining differences:
- mindmap: section colours, and the layout is still not cose-bilkent;
- gantt: mermaid draws denser ticks;
- state: concurrent regions are not drawn as grey boxes;
- class: wide single-row layout, same as mermaid's;
- journey: legend order;
- quadrant: mermaid 11.17 renders points with a broken `hsl(NaN)` fill, so cmermaid keeps blue;
- C4: arbitrary bgColor strings are rejected.
mermaid itself fails to render `block_full` and `radar_full` (partial named curve).

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

## v4/v5: layout quality

- Subgraph boxes never overlap: ordering uses one global key per cluster (mean member slot over all
  layers), so clusters stay contiguous in every layer; Brandes-Köpf keeps cluster border chains
  rigid (borders always align with their chain partner, segments crossing a chain are conflicts).
  Overlapping cluster pairs over the mmdr fixtures, bench inputs and tests: 12 → 0.
- Router: corners buried in obstacles are pruned with a single grid-cell lookup, and side detours
  are tested shortest-first (the first clear one wins).
- Timings (µs, one core; v1 = original): medium 404 (v1) / 534 (avoid on) / 406 (`--no-avoid`),
  xlarge 54862 / 54061 / 48932. Full numbers and the mmdr comparison: `bench/results_v4.txt`, `bench/results_v5.txt`.
- Known gap: long edges between rectangular clusters can take wide detours around them
  (e.g. `mega_event_mesh`); this comes from layout, not the router (`--no-avoid` shows the same).

## v6: speed + flowchart config parity
- Medium avoid-on 534 -> ~420 µs (v1 404); no-avoid 317 µs; large 8.3 ms (v1 8.9).
- xlarge is ~200 ms vs v1 54 ms: the round-5 DFS feedback arc set (mermaid's default acyclicer) gives
  far longer back-edge chains on the random xlarge input. Fidelity was chosen over speed here.
- Flowcharts now honour `accTitle`/`accDescr` (svg `<title>`/`<desc>` + aria) and `%%{init}%%`/frontmatter
  `themeVariables` (primaryColor, primaryBorderColor, primaryTextColor, lineColor, edgeLabelBackground,
  clusterBkg, clusterBorder, titleColor, background, fontFamily, ...) through the same d_cfg system as the other diagrams.
- Numbers and the mmdr comparison over all diagram types: `bench/results_v6.txt`.

## v7
- Hybrid cycle removal: DFS FAS by default, greedy (Eades) when N ≥ 1500 and DFS reverses ≥ N/20 edges.
  `flowchart.cycleRemoval: dfs|greedy|auto`, flags `CMM_F_FAS_DFS`/`CMM_F_FAS_GREEDY`. xlarge 50.5 ms (v1 53.8).
- `flowchart.nodeSpacing`, `flowchart.rankSpacing`, `flowchart.curve` (basis, linear, step, stepBefore, stepAfter, cardinal/catmullRom).
- Self-loops reserve side room in BK placement; multiple loop labels stack outside the outermost loop.
- Subgraph `direction` is ignored when member nodes link outside the subgraph (mermaid rule).
- Numbers: bench/results_v7.txt.

## v8
- Medium avoid-on 394.7 µs (v1 404.1): single ordering sweep for mid-size graphs (512 < segs ≤ 4096).
- `tests/bounds.py`: every case and repo fixture must keep all geometry (path control points, exact arcs,
  shapes, text anchors) inside the root viewBox; self-loop curves now included in the flowchart viewBox.
- Compound ordering seeded with dagre's tie order; same-rank order agreement with mermaid 0.175 → 0.936.
- Numbers: bench/results_v8.txt.

## v10

- Faithful dagre compound ranking in dagre mode (`src/drank.c`): nesting graph (border top/bottom nodes, nodeSep weights), network simplex (graphlib iteration order), label proxies, removeEmptyRanks, cleanup, border segments.
- Ordering agreement with mermaid: 1051/1051. Rank-axis position ordering vs mermaid SVGs: 100%. Cross-axis: 99.1% (`bench/posagree.py`).
- Small graphs use arena scratch in `dagre_order`. Small takes 16.9 µs in dagre mode, or 12.8 µs in fast mode. See `bench/results_v10.txt`.

## v9
- Faithful port of dagre's order step (`src/dorder.c`: initOrder DFS, layer graphs with cluster borders,
  barycenter/sortSubgraph/resolveConflicts with subgraph constraints, biased down/up sweeps, keep best by
  crossCount), reproducing graphlib key order. Default for graphs with V ≤ 512; the fast ordering is kept above.
  Override: `flowchart.order: dagre|fast` or env `CMM_ORDER`.
- Same-rank order agreement with mermaid 0.936 → 0.961 (macro 0.658 → 0.813). Numbers: bench/results_v9.txt.
