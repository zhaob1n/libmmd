# libmmd internals, per-type feature notes and version history

Numbers here are historical; see bench/results_v*.txt.

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
| writer | `src/writer.c`, writer section of `include/mmd.h` | builds mermaid text by escape context (entities `#NN;`), shortest round-trip numbers, typed helpers for eleven diagram types; per-diagram audit in [writer.md](writer.md) |
| stateDiagram(-v2) | `src/d_state.c` | composites (recursive per-scope layout), `--` regions, `[*]`, fork/join/choice, notes, classDef |
| classDiagram | `src/d_class.c` | members, `~generics~`, visibility/`$`/`*`, annotations, all relations + lollipop, cardinality, namespaces, notes |
| erDiagram | `src/d_er.c` | attribute tables (type/name/keys/comment), aliases, all crow's-foot cardinalities (symbolic + word forms), `:::cls` / classDef / class / style, `subgraph` clusters (relations may target a subgraph id) |
| pie | `src/d_pie.c` | slices, showData, title, legend |
| gantt | `src/d_gantt.c` | dayjs-style dateFormat (incl. Z/ZZ offsets), todayMarker (MMD_TODAY override), weekday/weekend friday, strftime axisFormat, tickInterval, excludes (weekends/dates), after/until deps, done/active/crit/milestone/vert, invalid tickInterval ignored, `gantt.useWidth` |
| journey | `src/d_journey.c` | sections, scored tasks with faces, actor legend/dots |
| mindmap | `src/d_mindmap.c` | indentation tree, all node shapes, radial layout (tidy-tree opt-in), `::icon(fa ...)` drawn as real Font Awesome paths (badge fallback), `:::class`, section colours |
| timeline | `src/d_timeline.c` | sections, periods, multi-event / continuation lines, wrapping |
| quadrantChart | `src/d_quadrant.c` | axes, quadrant labels, points with inline style and classDef |
| xychart(-beta) | `src/d_xychart.c` | vertical/horizontal, categories or numeric range, bar + line series, nice ticks, legend for titled series; `xyChart.showDataLabel` / `showDataLabelOutsideBar` (mermaid 11: value inside the bar, or past its end; each bar uses its own value, outside labels stay 12px); `xyChart.barOverlap` (libmmd only: bar series share the full band, a 0 draws no bar and no label — one series per group colours the bars; mermaid.js ignores the key and draws them side by side); vertical category labels wider than their band wrap at spaces, and the plot grows for the extra lines |
| gitGraph | `src/d_gitgraph.c` | LR/TB/BT, branch order, merge, cherry-pick, tags, NORMAL/REVERSE/HIGHLIGHT, lane-routed merges, mermaid lane order (unordered branches = `0.<index>`), hashed auto ids |
| sankey(-beta) | `src/d_sankey.c` | RFC-4180 CSV, longest-path columns (sinks justified), barycentric relaxation, value labels with collision avoidance |
| block(-beta) | `src/d_block.c` | columns, spans, `space`, nested blocks, 10 shapes, labelled edges clipped to the true shape outline; grid layout |
| requirementDiagram | `src/d_requirement.c` | 6 requirement kinds + element, all 7 relations (grey label plates), `direction`; layered layout |
| C4* | `src/d_c4.c` | Context/Container/Component/Dynamic/Deployment, Person/System/Container/Component(+Db/Queue/_Ext), boundaries, Rel variants, `c4ShapeInRow`/`c4BoundaryInRow` grid, UpdateLayoutConfig/UpdateElementStyle/UpdateRelStyle (colours whitelisted), mermaid-11 person shape + `[Type: techn]` line |
| architecture(-beta) | `src/d_architecture.c` | groups (nested), services, junctions, port-constraint grid, orthogonal edges, non-overlapping groups, built-in icons + embedded `logos:`/`fa:` subset (src/icons_data.h via tools/genicons.js: simple-icons CC0, FA Free CC-BY-4.0) |
| packet(-beta) | `src/d_packet.c` | bit ranges, `+N`, row splitting, bit numbers, title below (as mermaid) |
| radar(-beta) | `src/d_radar.c` | axes, positional/named curves, max/min/ticks, polygon graticule, legend |
| treemap(-beta) | `src/d_treemap.c` | squarified layout, sections, classDef/`:::`; values through `treemap.valueFormat` (default `,`) and mermaid's `$` shorthands, by `src/d3fmt.c`: d3-format 3.1 in the en-US locale, checked against the real d3 on 5216 spec/value pairs (`tests/d3fmt_cases.tsv`, regenerate with `node tools/d3fmt_cases.mjs`) |
| kanban | `src/d_kanban.c` | columns, cards, `@{assigned, ticket, priority}`; named themes colour columns like mermaid (cScale[n+2] lightened/darkened 10%, cScaleLabel titles) |
| ishikawa | `src/d_ishikawa.c` | fishbone port of mermaid's renderer: alternating up/down causes, nested sub-causes along bones, wrapped head |
| cynefin | `src/d_cynefin.c` | five domains, items (confusion capped at 3 + overflow badge), transitions; seeded wavy boundaries match mermaid-cli byte-for-byte |
| ZenUML | `src/d_zenuml.c` | native renderer in ZenUML's look: titled outer frame, boxed participant headers with `@Actor/@Database/@Boundary/@Control/@Entity/@Collections/@Queue` icons, unnamed starter column (or `@Starter(X)`), hierarchical message numbers (1, 2.1, 2.1.1), `// comments` above the next message, sync calls with `{}` bodies (activations, explicit `return` plus `x =` assignment return), `new X()` creating an `x:X` box mid-diagram, async `A->B: msg` / dashed `A-->B`, Alt/Loop/Opt/Par/Try/Critical/Section fragments (if/else if/else, while/for/forEach/loop, try/catch/finally) with numbered headers, ZenUML-style header icons and condition rows, title |
| config / a11y | `src/d_cfg.c`, `src/dcfg.h` | `%%{init}%%` (relaxed JSON) + YAML frontmatter `config:` flattened to dotted keys; a directive's `config` object lands where mermaid's detectInit files it, under the detector id: the diagram's section, except for class, state, xychart, flowchart-elk and the railroad dialects, whose ids no renderer reads, and nowhere when there are several directives; and `theme`/`look` resolve as mermaid 12's resolveAppearance does: the section's (`flowchart.theme`, `sequence.look` ...) before the top-level one, names mermaid does not know passed over (the caller's `mmd_options.theme` wins over both); `nodeSpacing`/`rankSpacing` as mermaid's dagre layout reads them (`dcfg_spacing`): a top-level one first, then the diagram's section, which is `state` for class and requirement diagrams too (their own sections are ignored), 0 and negatives as unset; ~85 themeVariables mapped to CSS per type (values whitelisted, `url(` rejected); named themes `dark`/`forest`/`neutral` for every non-flowchart type (presets generated by `tools/gen_themes.mjs` from mermaid's own theme objects into `src/d_themes.h`, incl. pie1..12/cScale/git palettes; user themeVariables override); per-diagram keys (pie.textPosition, timeline.disableMulticolor, sankey.showValues, quadrantChart.chartHeight, gantt.axisFormat/tickInterval, gantt.barHeight/barGap/topPadding/leftPadding, quadrantChart.chartWidth, xyChart.width/height, xyChart.showDataLabel/showDataLabelOutsideBar/barOverlap, sankey.width/height, packet.bitsPerRow); accTitle/accDescr (incl. `{}` block) → `<title>`/`<desc>` + aria-labelledby/describedby |
| text metrics | `src/text.c` | embedded Helvetica-compatible advance table, Unicode East Asian Width for CJK, 8-byte ASCII fast path |
| venn | `src/d_venn.c` | sets, unions with sizes, indented `text` nodes, `style`; venn.js-style area-proportional layout (lens-area bisection + stress descent), labels at each region's deepest point |
| treeView | `src/d_treeview.c` | indentation or box-drawing (`├──`/`└──`/`│`) trees, `:::highlight`, `icon(pack:name)` / showIcons file+folder icons, filename/extension icon maps, `## descriptions` |
| swimlane-beta | `src/d_swimlanes.c` (+ flowchart parser/drawer) | full-width lanes from top-level subgraphs (LR/RL rows with rotated titles, TB/BT columns), lane-aware ranking (unlabelled cross-lane hops share a column), orthogonal edge routing, all flowchart node shapes/styles |
| eventmodeling | `src/d_eventmodeling.c` | `tf`/`timeframe`, `rf`/`resetframe` frames (ui, cmd, evt, rmo, pcr + long names), `Ns.Name` stream/namespace lanes, `->> id` sources, inline `{ ... }` and `[[data]]` blocks, mermaid box sizing and relation geometry, `em*` theme variables |
| railroad | `src/d_railroad.c` | `railroad-ebnf-beta` (`=`/`::=`, `\|`, `[ ]`, `{ }`, `?*+`, `-` exception, `? special ?`, `(* *)`), `railroad-abnf-beta` (`/`, `n*m` repeats, `%x` values), `railroad-peg-beta` (`<-`, `/`, `&`/`!` lookahead, `.`), `railroad-beta` function DSL; mermaid arc geometry |
| wardley | `src/d_wardley.c` | `wardley-beta`: `title`, `size`, `evolution` stages (`@bounds`, `A / B` labels), `anchor`, `component` (`label [x, y]`, `(build\|buy\|outsource\|market\|inertia)`), `pipeline { }`, `evolve`, `note`, `(de)accelerator`, `annotation(s)`, links `->` `+>` `+<` `+<>` `-.->` `+'lbl'>` `; label` |

## Docs syntax corpus

`tests/docs/<doc>/NN.mmd`: all 432 ```` ```mermaid ```` examples from mermaid's `packages/mermaid/src/docs/syntax/*.md` (mermaid 6d8921f, extracted by `tools/extract_docs.py`). `python3 tools/docs_audit.py [-v] [--mmdc]` reports per-type pass counts against mermaid-cli (cached in `tests/docs/mermaid_status.txt`); `tools/docs_sbs.py` renders side-by-sides. Every type passes 100% of the examples mermaid itself renders (432/432; examples mermaid rejects would be excluded). `make test` runs the audit and fails on any regression.

## Fidelity vs mermaid-cli 11.17

Side-by-side renders (left: libmmd, right: mermaid 11.17) live in `samples/sbs/*.png`.
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
- quadrant: mermaid 11.17 renders points with a broken `hsl(NaN)` fill, so libmmd keeps blue;
- C4: arbitrary bgColor strings are rejected.
mermaid itself fails to render `block_full` and `radar_full` (partial named curve).

### mermaid 12 defaults (checked against mermaid-cli 12.0.0)

mermaid 12's config schema gives flowchart, swimlane, sequence, class, state, ER, requirement,
usecase, venn and agentflow `look: neo` and `theme: redux-color`, and the global `layout` is `elk`
(bundled, so flowcharts are laid out by ELK unless `layout: dagre`). `look: classic` alone keeps the
redux palette; the classic look needs a classic theme too (`dcfg_neo()`), named in the source or by
the caller: `mmdc -t default` and `mmd -t default` are classic, no theme at all is neo. Side by side, the neo
flowchart (120px minimum label boxes, 171px diamonds for one-word labels, a monospace-looking font)
is roomier without being clearer, so flowcharts stay classic and dagre. agentflow and usecase only
exist in mermaid 12 and follow its neo default; `look: classic` with a classic theme switches them.
usecase draws the front-matter `title`. mermaid 12's renderer means to (`insertTitle` with
`usecaseDiagramTitleText`), but its parser calls `db.clear()` first and wipes the title the front matter
set, so mermaid-cli 12.0.0 shows none. The classic look takes every colour mermaid's usecase styles read
(`primaryTextColor`, `actorTextColor`, `titleColor`, `note*Color`, `actorBkg`/`actorBorder`, the
`usecase*` overrides) from the theme; checked role by role against mermaid-cli 12.0.0 in all four themes.
Compare libmmd's flowcharts with mermaid 12 under `look: classic`, `theme: default`, `layout: dagre`:
on the nested-cluster cases checked, ranks and ordering then agree and libmmd's cluster padding is the tighter of the two.

## Flowchart features (v2)
- `direction` inside subgraphs: clusters with their own direction are laid out recursively and placed as
  fixed-size proxy nodes; edges to a subgraph id attach to the cluster box; cluster titles are moved
  sideways to avoid edges crossing the title band.
- Crossing reduction: barycenter sweeps + keep-best + budgeted transpose polish (dirty-layer skipping).
- Port ordering for fan-in/fan-out and parallel edges; nested multiple self-loops.
- `mmd --avoid` (`MMD_F_ROUTE_AVOID`): detours polyline segments around nodes they pass through.
- Themes: `default`, `dark`, `forest`, `neutral` via `%%{init: {"theme": ...}}%%`, frontmatter, or `-t`.
- Markdown strings (`"`**bold** *italic*`"`) with auto-wrap at 200px, `fa:fa-name` icons
  (Font Awesome glyphs; needs the font installed to render), edge ids `A e1@--> B`, `e1@{ animate: true }`.
- Golden tests in `tests/golden` (`UPDATE_GOLDEN=1 bash tests/run_tests.sh` to refresh).
- Benchmarks: `bench/results_v2.txt`.

## v3: build options, batch API, routing

- Build: `make` (portable `-O2`), opt-in `make MARCH=native`, `make LTO=1`, `make pgo [LTO=1 MARCH=native]` (output in `build-pgo/`).
- Shared library: `build/libmmd.so.0.3.0` (soname `libmmd.so.0`, only `mmd_*` exported) plus `build/mmd.pc`; `make install PREFIX=/usr DESTDIR=...`.
- Batch rendering: `mmd_context_new()` / `mmd_context_render_svg()` / `mmd_context_free()` reuse the arena and output buffer between calls.
- Obstacle routing is on by default: edges that cross a non-endpoint node get a local visibility-graph (A*) detour. Use `--no-avoid` / `MMD_F_NO_AVOID` for the fastest layout. Edge labels sit on the rendered curve.
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
  `flowchart.cycleRemoval: dfs|greedy|auto`, flags `MMD_F_FAS_DFS`/`MMD_F_FAS_GREEDY`. xlarge 50.5 ms (v1 53.8).
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
  Override: `flowchart.order: dagre|fast` or env `MMD_ORDER`.
- Same-rank order agreement with mermaid 0.936 → 0.961 (macro 0.658 → 0.813). Numbers: bench/results_v9.txt.

## v11
Mermaid-exact text metrics (Liberation/Arial advances+kerning), mermaid node padding, self-loop helper nodes. See bench/results_v11.txt.

## v12

- Node-centre error vs mermaid: 67.7 px -> 1.61 px mean (median 0, p90 6.1). Sources fixed: double cluster
  padding in dagre mode, dagre ordering threshold (now N<=256), exact edge-label bbox sizes, and dagre's
  labelRank (proxy rank on nesting-graph ranks before removeEmptyRanks).
- Small render 15.0 us (target 11.7 us not met); medium 590 us (dagre-exact ordering now applies).
- bench/eq/: byte-identity harness. See bench/results_v12.txt.

## v13 (speed)
Byte-identical output. medium 590.5 → 481.5 µs, small 15.1 → 14.6 µs (min, this box). v1 is at 407 / 11.9, so the targets are not met yet.
Main gains came from incremental network simplex (rank cycles 449k → 231k) and from dorder sort/crossing shortcuts.
Build with `-DMMD_PROFILE` to get per-phase cycle counters. See bench/results_v13.txt and bench/prof13/.

## Toolchain
- Builds use `-std=gnu2y -fms-extensions -Wno-microsoft-anon-tag` with clang and gcc (`make both`; developed on clang 23 and gcc 16). Code must compile warning-free under both. Where no distro ships a recent gcc, a C-only build is quick: `configure --enable-languages=c --disable-bootstrap --disable-multilib --program-suffix=-16` (about 5 minutes on 8 cores). `gcc-16 -fanalyzer` is clean on `src/`; `clang --analyze` only reports leaks through `[[gnu::cleanup]]` owners, which it does not model.
- Scripts run from any directory and assume no machine layout: mermaid-cli, its puppeteer config and Chrome come from `MMDC`, `MMDC_PUPPETEER` and `CHROME` or `PATH` (`tools/env.py`), scratch files go under `TMPDIR`.
- Byte-identity gate: `bench/eq/cmp.sh build/mmd` must print `bad=0`.
- Before writing a string helper, check `src/base.h` (`str_sub`/`str_from`/`str_span`, `str_split`, `str_cat`/`STR_CAT`, `buf_cstr`), `src/text.h` (`label_br_len`) and `src/dgraph.h` (`dg_unquote`, `dg_css_ok`, `dg_arrowhead`). Literal config keys go through `dcfg_get_str(S("..."))`.
- `restrict` is a caller contract, not decoration: use it only where every caller provably passes disjoint objects (Buf appends vs. their source, independent scratch arrays). Never append a Buf into itself — `buf_reserve` may move it. `str_cat`, `str_split` and `arena_grow` stay unqualified (their inputs and results may legitimately share storage), and the public `include/mmd.h` stays unrestricted.
- Resource release uses `[[gnu::cleanup]]` via `src/base.h`: `AUTOFREE` (free), `AUTO_BUF` (Buf), `AUTO_ARENA` (Arena), `AUTOCLOSE` (FILE). No hand-written frees or goto-cleanup; return ownership with `TAKE_PTR(p)` / `buf_take(&b)`. Never `goto` past a cleanup variable's declaration (clang errors, gcc silently skips it): declare them before the first jump.
- Diagram code may format with printf `%g`/`%.2f` and read config numbers with `strtod`: `render()` in `src/mmd.c` switches the thread to a cached C locale for the whole render (about 5 ns), so none of them needs a locale-free twin. Code outside a render takes the same switch where it needs one: `c_locale_enter()` in `src/base.h`. `tests/locale_test.c` guards it.
- Output must not depend on earlier renders in the same process: arena memory is not zeroed (`ANEW`), and fresh chunks only look zero. MemorySanitizer finds reads before writes in fresh memory (`tools/sancheck.sh`), `tests/poison.sh` the ones that a zero fill hides, and the locale test's reused context the ones that only stale data from an earlier render exposes.
