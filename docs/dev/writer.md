# mmd_writer: escape contexts and the per-diagram audit

`include/mmd.h` (writer section), `src/writer.c`. Tested by `tests/writer_test.c` (`make test`); timed by
`bench/writer_bench.c` (`make build/bench-writer`).
mermaid.js is the reference: where libmmd and mermaid disagree on what a text means, mermaid wins.

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
  cover xychart, pie, flowchart, sequence, gantt, state, sankey, mindmap, treeView, ER and gitGraph:
  they know what the contexts can't, a diagram's keywords and id rules.
- **One header.** The writer is the second half of `include/mmd.h`, not a header of its own.
- **Blocks and depth live in the writer.** Helpers that open a block (sequence `loop`/`alt`/`par`/...,
  `mmd_state_group`, `mmd_er_entity`) push it; `mmd_end()` writes `end` or `}`; `else`/`and`/`option`
  must be inside their `alt`/`par`/`critical`; text and render refuse a block left open. Mindmap
  and treeView nodes take a depth that may grow by one per node.
- **Keyword collisions: entity, not error.** mermaid's jison lexers take the first rule that
  matches, without a word boundary, so a sequence participant `endpoint` lexes as `end`, a gantt
  task `section 2` or `2026-05-01 launch` as a section or a date. The helpers write such a value's
  first character as an entity (`#101;ndpoint`); it renders the same and every mention made through
  the helpers is spelled the same way. Where the grammar has no such escape (state ids, gitGraph
  branch names, gantt task ids) the value is refused.
- **Numbers**: `buf_num()` is fixed 2-decimal and clamps |v| > 1e15 to 0: fine for SVG
  coordinates, lossy for data (0.0012 s → 0). `mmd_w_num` writes the shortest decimal that reads
  back exactly, never with an exponent (mermaid's number tokens have none). 1e-4 ≤ |v| < 2^53 uses
  exact 128-bit interval arithmetic (~50 ns); other magnitudes a `%.*e` search (rare, ~1 µs).
  `tests/writer_test.c` checks round trip and minimality on 10⁴ values of every magnitude.

## Escapes

| | quoted `"…"` | text to end of line | CSV |
|---|---|---|---|
| always `#NN;` | `" : [ ] { } \| < # % \ * = `` ` `` | `" : [ ] { } \| < # % \ * ; , ~` | never (refused instead) |
| `_` | at a word edge | same | |
| `-` | | before `-` `>` `.` | |
| `/` | | before `/` | |
| `\n` | `<br>` with `MMD_W_BR`, else refused | same | refused |
| control bytes, bad UTF-8 | refused | refused | refused, and tab |

CSV: mermaid's sankey grammar reads printable ASCII and runs no entity decoding past its lexer, so
non-ASCII, tab, `<` `>` (shown as `&lt;` or dropped), `#name;` and `%%{` (a directive) are refused.
Fields are quoted (`""` for `"`) when they hold `,` `"`, edge blanks or a leading `%`.

Extra escapes the typed helpers add:
- treeView names: `(` (`icon(…)`), a trailing `/` (folder), a leading `"` or `'` (strings),
  `title`/`accTitle`/`accDescr`, and every box-drawing character (one `├` anywhere switches
  mermaid to box-drawing indentation);
- mindmap nodes and states without a label, whose id holds `_`: the id is written as a quoted label
  too (bare text is markdown);
- state labels and ER names and labels: the `d` of `direction` before a blank (`.*direction\s+LR`
  anywhere on the line sets the layout direction);
Identifiers: `[A-Za-z0-9_]` and non-ASCII UTF-8 up to U+FFFF (mermaid's lexers take no emoji),
non-empty; `mmd_flow_*` also refuse flowchart keywords (`end`, `subgraph`, `class`, `style`, ...).
Leading/trailing blanks are trimmed by every parser; that is not reported.

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
- ` %%` starts a comment anywhere on a `dg_next_line()` line; mermaid's timeline takes a `%` at a
  line start as one, and finds `%%{` directives anywhere.
- `#` starts a comment in mermaid's sequence, journey and timeline lexers (`\#[^\n]*`).
- `\` is an escape in mermaid's quoted strings (pie, gitGraph, flowchart `@{label}`, ER).
- `*` and `_` at a word edge are markdown emphasis in mindmap, ER and state text.
- `=` in a quoted gitGraph commit id or tag shows as `&equals;`.

Fixed in the parsers along the way: an entity's `;` at line end was stripped (`dg_next_line`,
sequence) or split the ZenUML statement; `A -- "a--b" --> B` ended the label inside the quotes;
quadrant axes kept their quotes; sankey lost a trailing ` %%…` or `;` of a field (mermaid's CSV
grammar has no comments).

## Audit: slots per diagram type

Q = `mmd_w_str`, T = `mmd_w_text`, I = `mmd_w_id`, C = `mmd_w_csv`; **br** = draws `<br>` (in
mermaid).
Every listed slot is in `tests/writer_test.c` and survives random labels built from all of the
above (CJK, emoji, `#quot;`, `%%`, `-->`, `fa:fa-car`, newlines...).

| type | Q | T | I | notes |
|---|---|---|---|---|
| flowchart, swimlane-beta | node shapes, `@{label}`, `-->\|…\|`, `-- … -->`, subgraph `[…]` (br) | | node ids | bare shape text breaks on brackets: use Q |
| sequenceDiagram | | message, note, `as` alias (br); loop/alt, title | participants | `mmd_seq_*`: ids that start with a keyword (`end`, `and`, `par`, `over`, ...) or a digit get an entity first character |
| classDiagram | `class A[…]`, note (br) | relation label (br) | class names | member lines are member syntax (`+`, `$`, `*`, `~T~`, `()`): `mmd_w_raw` |
| stateDiagram | `state … as A` (br) | description, transition, note (br) | state ids | `mmd_state_*` refuse keyword ids (`state`, `note`, `class`, ...) |
| erDiagram | relation, attribute comment, alias `p[…]` | | entities | `mmd_er_*` quote every name, so `-`, blanks and keywords (`one`, `to`) are fine |
| pie | slice | title | | |
| gantt | | task, section, title | | `mmd_gantt_task` writes dates and `Nd` durations, refuses tag-like ids |
| journey | | task, section (br); actor | | |
| timeline | | period, event, section (br) | | |
| mindmap | shape `id[…]` (br) | | node ids | bare node lines are node specs (`[`, `(`, `:::`, `::icon`); `mmd_mindmap_node` nests by depth |
| quadrantChart | point, quadrant, axis | title | | mermaid's unquoted quadrant/axis text rejects `( ) @ ^ > ~` and more |
| xychart | title, axis titles, categories, series | | | |
| gitGraph | commit `id:`, `tag:` | | branches | `mmd_git_*` check branch names against `\w([-./\w]*[-\w])?` and the keywords |
| sankey | | | | C: source, target (`mmd_sankey_*`) |
| block | node, edge | | | |
| requirementDiagram | `text:`, `type:` | | | |
| C4 | label, description (br); `Rel` label | title | | mermaid shows entities literally (`&#58;`) |
| architecture | | `[…]` of service, group, edge | | mermaid shows entities literally and lexes few characters |
| packet | field | title | | |
| kanban | `[…]` card, column (br) | | | nest with `mmd_w_line` |
| radar | axis and curve `id[…]` | title | | |
| treemap | leaf, section | | | nest with `mmd_w_line` |
| zenuml | | message, title | | mermaid's zenuml plugin shows entities literally |
| ishikawa | | head, causes (br) | | nest with `mmd_w_line` |
| cynefin | item, transition | title | | |
| venn | set, text `[…]` | | | |
| treeView | | `## description` | node names | `/` (folder), `:::`, `icon(` are syntax in names; `mmd_tree_node` escapes them |
| eventmodeling | | `data` values | names | names are dotted identifiers; mermaid: ASCII, letter first |
| railroad | terminals (EBNF, function form) | | | `title` is accepted but not drawn, as in mermaid-cli |
| wardley | note | component, title | | mermaid's component lexer takes no entity |

## Not representable (refused or documented)

- A line break outside a **br** slot (pie, xychart, gantt, sankey, ...): refused.
- An empty xychart category or series list: mermaid's grammar needs one item, so it is refused.
- Leading/trailing blanks: trimmed by every parser.
- T at the start of a statement can't start with that diagram's keyword (`section`, `title`, ...).
  The typed helpers handle it (gantt tasks, sequence ids, treeView names); written by hand with
  `mmd_w_text` (timeline period, ishikawa cause, wardley component) it is not checked.
- mermaid.js limits no escape gets around: C4 and architecture print entities literally, as does the
  zenuml plugin; wardley components, eventmodeling names and gitGraph branches take a narrow
  character set; a literal `<br>` or `fa:fa-x` is drawn as markup whatever its spelling; mermaid
  hyphenates long unspaced sequence block labels, sometimes inside an escape; sankey, gitGraph and
  xychart show `<` `>` as `&lt;` `&gt;`; xychart omits some series names; a decoded `\` before
  punctuation in markdown text (class notes, mindmap, ER, state) escapes it.
