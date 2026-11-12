# mmd_writer: escape contexts and the per-diagram audit

`include/mmd_writer.h`, `src/writer.c`. Tested by `tests/writer_test.c` (`make test`); timed by
`bench/writer_bench.c` (`make build/bench-writer`).

## Decisions

- **Text is the interface.** No public AST: the writer appends to a `Buf`, `mmd_writer_render_svg()`
  is `mmd_render_svg()` of that exact text, so a saved `.mmd` re-renders byte-identically and no
  diagram's internal structs become ABI.
- **Sticky error** (like `ferror`). The first value a context can't carry sets `MMD_ERR_SYNTAX`, a
  message and the text offset; later calls are no-ops. `include/mmd.h` is untouched, so no new
  status code. A failed `mmd_writer_new()` (nullptr) behaves as an out-of-memory writer.
- **One escape for every diagram: mermaid entities.** mermaid.js replaces `#name;` / `#123;` before
  any grammar runs; libmmd decodes them in `buf_xml()` and measures them as one glyph in
  `text_width()`, on every text path. So any syntax character becomes `#NN;`, and the writer only
  has to know *which* characters a context needs escaped, not how each diagram unescapes.
- **Contexts, not diagrams.** `mmd_w_str` (quoted), `mmd_w_text` (to end of line), `mmd_w_id`,
  `mmd_w_num`, `mmd_w_csv`, `mmd_w_line` (nesting by indentation), `mmd_w_raw`. Typed helpers
  exist where the diagram is data: xychart, pie, flowchart.
- **Numbers**: `buf_num()` is fixed 2-decimal and clamps |v| > 1e15 to 0: fine for SVG
  coordinates, lossy for data (0.0012 s → 0). `mmd_w_num` writes the shortest decimal that reads
  back exactly, never with an exponent (mermaid's number tokens have none). 1e-4 ≤ |v| < 2^53 uses
  exact 128-bit interval arithmetic (~50 ns); other magnitudes a `%.*e` search (rare, ~1 µs).
  `tests/writer_test.c` checks round trip and minimality on 10⁴ values of every magnitude.

## Escapes

| | quoted `"…"` | text to end of line | CSV |
|---|---|---|---|
| always `#NN;` | `" : [ ] { } \| < `` ` `` | `" : [ ] { } \| < ; , ~` | `<` |
| `#` | before `\w+;` | same | same |
| `%` | before `%` | same | same |
| `-` | | before `-` `>` `.` | |
| `/` | | before `/` | |
| `\n` | `<br>` with `MMD_W_BR`, else refused | same | refused |
| control bytes, bad UTF-8 | refused | refused | refused |

CSV fields are quoted (`""` for `"`) when they hold `,` `"`, edge blanks or a leading `%`.
Identifiers: `[A-Za-z0-9_]` and non-ASCII UTF-8, non-empty; `mmd_flow_*` also refuse flowchart
keywords (`end`, `subgraph`, `class`, `style`, ...). Leading/trailing blanks are trimmed by every
parser; that is not reported.

Why each one (raw probes against every slot below; `#` columns are the parser that breaks):
- `"` ends quoted strings everywhere; unquoted it is stripped by `dg_unquote`.
- `:` splits gantt/journey/timeline/quadrant/sequence-title lines, and quadrant and treemap find
  `:` / `:::` without honouring quotes; `fa:fa-x` becomes an icon in every `dg_tspans()` text.
- `]` `}` `|` close `[…]`, `@{…}`, `|…|` and venn/radar/architecture labels even inside quotes;
  `{` breaks class labels, `{ }` zenuml blocks; `[` quadrant points.
- `;` separates statements (sequence, zenuml) and is stripped at line end (`dg_next_line`).
- `,` splits journey actors; `~` is class generics; `-->` `--` `..` turn state/class text into
  relations; `//` starts a zenuml comment; `` ` `` opens a markdown string inside quotes.
- `<br>` is a line break wherever labels are normalised, so `<` is always escaped.
- ` %%` starts a comment anywhere on a `dg_next_line()` line.

Fixed in the parsers along the way: an entity's `;` at line end was stripped (`dg_next_line`,
sequence) or split the ZenUML statement; `A -- "a--b" --> B` ended the label inside the quotes.

## Audit: slots per diagram type

Q = `mmd_w_str`, T = `mmd_w_text`, I = `mmd_w_id`, C = `mmd_w_csv`; **br** = draws `<br>`.
Every listed slot is in `tests/writer_test.c` and survives random labels built from all of the
above (CJK, emoji, `#quot;`, `%%`, `-->`, `fa:fa-car`, newlines...).

| type | Q | T | I | notes |
|---|---|---|---|---|
| flowchart, swimlane-beta | node shapes, `@{label}`, `-->\|…\|`, `-- … -->`, subgraph `[…]` (br) | | node ids | bare shape text breaks on brackets: use Q |
| sequenceDiagram | | message, note, `as` alias (br); loop/alt, title | participants | |
| classDiagram | `class A[…]`, note (br) | relation label (br) | class names | member lines are member syntax (`+`, `$`, `*`, `~T~`, `()`): `mmd_w_raw` |
| stateDiagram | `state … as A` (br) | description, transition, note (br) | state ids | |
| erDiagram | relation, attribute comment, alias `p[…]` | | entities | ER also allows `-` in names; `mmd_w_id` does not |
| pie | slice | title | | |
| gantt | | task, section, title | | |
| journey | | task, section (br); actor | | |
| timeline | | period, event, section (br) | | |
| mindmap | shape `id[…]` (br) | | node ids | bare node lines are node specs (`[`, `(`, `:::`, `::icon`); nest with `mmd_w_line` |
| quadrantChart | point | quadrant (br); axis, title | | |
| xychart | title, axis titles, categories, series | | | |
| gitGraph | commit `id:`, `tag:` | | branches | the parser rejects quoted branch names with blanks |
| sankey | | | | C: source, target |
| block | node, edge | | | |
| requirementDiagram | `text:` | `type:` | | |
| C4 | label, description (br); `Rel` label | title | | |
| architecture | | `[…]` of service, group, edge | | |
| packet | field | title | | |
| kanban | `[…]` card, column (br) | | | nest with `mmd_w_line` |
| radar | axis and curve `id[…]` | title | | |
| treemap | leaf, section | | | nest with `mmd_w_line` |
| zenuml | | message, title | | |
| ishikawa | | head, causes (br) | | nest with `mmd_w_line` |
| cynefin | item (br), transition | title | | |
| venn | set, text `[…]` (br) | | | |
| treeView | | `## description` | node names | `/` (folder), `:::`, `icon(` are syntax in names |
| eventmodeling | | `data` values | names | names are dotted identifiers |
| railroad | terminals (EBNF, function form) | | | `title` is accepted but not drawn |
| wardley | note | component, title | | |

## Not representable (refused or documented)

- A line break outside a **br** slot (pie, xychart, gantt, sankey, ...): refused.
- An empty xychart category or series list: mermaid's grammar needs one item, so it is refused.
- Leading/trailing blanks: trimmed by every parser.
- T at the start of a statement (gantt task, timeline period, ishikawa cause, wardley component)
  can't start with that diagram's keyword (`section`, `title`, ...). The writer can't know the
  diagram, so this is not checked; typed helpers should.
- libmmd's `dg_parse_num` accumulates digit by digit, so a rendered value can differ from the
  written one in the last ulp (invisible); mermaid.js reads it with `parseFloat`.
