/* libmmd — fast Mermaid renderer in modern C. Public API: rendering, then the source writer. */
#ifndef MMD_H
#define MMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MMD_API [[gnu::visibility("default")]]

/* ---- rendering: Mermaid text to SVG ---- */
typedef enum [[nodiscard]] mmd_status {
    MMD_OK = 0,
    MMD_ERR_SYNTAX,      /* parse error, see mmd_error line/col */
    MMD_ERR_UNSUPPORTED, /* diagram type not implemented */
    MMD_ERR_NOMEM,
    MMD_ERR_EMPTY,       /* no diagram found */
    MMD_ERR_LIMIT,       /* input exceeds internal limits */
} mmd_status;

typedef struct mmd_error {
    mmd_status code;
    uint32_t line; /* 1-based, 0 if unknown */
    uint32_t col;  /* 1-based, byte column */
    char message[160];
} mmd_error;

typedef struct mmd_buffer {
    char *data; /* NUL-terminated, owned; free with mmd_buffer_free */
    size_t len;
} mmd_buffer;

typedef enum mmd_theme {
    MMD_THEME_AUTO = 0, /* from %%{init}%% / frontmatter, else default */
    MMD_THEME_DEFAULT, MMD_THEME_DARK, MMD_THEME_FOREST, MMD_THEME_NEUTRAL,
} mmd_theme;

enum {
    MMD_F_ROUTE_AVOID = 1u, /* (default since v3; kept for compatibility) */
    MMD_F_NO_AVOID = 2u,    /* flowchart: disable re-routing edges around non-incident nodes */
    MMD_F_FAS_DFS = 4u,     /* cycle removal: always mermaid/dagre DFS (default: auto hybrid) */
    MMD_F_FAS_GREEDY = 8u,  /* cycle removal: always greedy (Eades) */
};

typedef struct mmd_options {
    double node_spacing; /* 0 => default (50) */
    double rank_spacing; /* 0 => default (50) */
    double font_size;    /* 0 => default (16) */
    mmd_theme theme;     /* 0 => from source / default */
    uint32_t flags;      /* MMD_F_* */
} mmd_options;

/* Render Mermaid source (n bytes, need not be NUL-terminated) to SVG.
 * opt may be nullptr. err may be nullptr. On success *out owns a buffer. */
MMD_API mmd_status mmd_render_svg(size_t n, const char *src,
                                        const mmd_options *opt,
                                        mmd_buffer *out, mmd_error *err);

MMD_API void mmd_buffer_free(mmd_buffer *buf);

/* Reusable render context for batch rendering: keeps its arena and output
 * buffer between calls so steady-state renders do (almost) no malloc. */
typedef struct mmd_context mmd_context;
[[nodiscard]] MMD_API mmd_context *mmd_context_new(void);
MMD_API void mmd_context_free(mmd_context *ctx);
/* On success *svg (NUL-terminated, *len bytes) is owned by ctx and stays
 * valid until the next call on ctx or mmd_context_free. Not thread-safe per
 * ctx; use one context per thread. */
MMD_API mmd_status mmd_context_render_svg(mmd_context *ctx, size_t n, const char *src,
                                                const mmd_options *opt, const char **svg, size_t *len,
                                                mmd_error *err);

MMD_API const char *mmd_status_str(mmd_status s);

/* ---- writer: Mermaid source text ----
 * The text it builds is the only interface: mmd_writer_text() hands it out (to save as .mmd), mmd_writer_render_svg()
 * renders that same text, so a saved file re-renders byte-identically.
 *
 * Errors are sticky, like ferror(): no call needs checking. A value a
 * context cannot carry (a quote in an identifier, a newline where the
 * diagram draws no line breaks, NaN, invalid UTF-8, ...) makes every later
 * call a no-op, and the text/render call reports it. Strings are UTF-8 and
 * NUL-terminated; a nullptr string where one is optional means "none".
 * Escaping and the per-diagram rules behind it: docs/dev/writer.md. */
typedef struct mmd_writer mmd_writer;

[[nodiscard]] MMD_API mmd_writer *mmd_writer_new(void);
MMD_API void mmd_writer_free(mmd_writer *w);
/* empty the text and clear the error; keeps the memory */
MMD_API void mmd_writer_reset(mmd_writer *w);

/* *s (NUL-terminated, *len bytes) stays valid until the next call on w.
 * On error *s is nullptr and err (nullable) says which call failed and where
 * in the text (line/col) its value would have gone. */
MMD_API mmd_status mmd_writer_text(const mmd_writer *w, const char **s, size_t *len, mmd_error *err);
/* mmd_render_svg() of exactly that text */
MMD_API mmd_status mmd_writer_render_svg(const mmd_writer *w, const mmd_options *opt,
                                                       mmd_buffer *out, mmd_error *err);

/* ---- escape contexts: work in every diagram type ---- */
enum {
    MMD_W_BR = 1u,        /* '\n' becomes <br>; only where the diagram draws line breaks */
    MMD_W_LINESTART = 2u, /* mmd_w_text at the start of a statement (timeline period, ishikawa cause,
                             wardley component ...): a leading keyword of the diagram becomes an entity */
};
/* end the current line (if any) and indent depth levels: statements, and the
 * nesting of mindmap, treeView, treemap, kanban, ishikawa, venn ... */
MMD_API void mmd_w_line(mmd_writer *w, unsigned depth);
/* verbatim: keywords, arrows, punctuation */
MMD_API void mmd_w_raw(mmd_writer *w, const char *s);
/* "…": a quoted label, e.g. A[…], pie …: 1, x-axis […], Person(p, …) */
MMD_API void mmd_w_str(mmd_writer *w, const char *s, unsigned flags);
/* label running to the end of the line, e.g. A->>B: …, title …, section … */
MMD_API void mmd_w_text(mmd_writer *w, const char *s, unsigned flags);
/* node / participant / entity id: [A-Za-z0-9_] and non-ASCII up to U+FFFF (no emoji), non-empty */
MMD_API void mmd_w_id(mmd_writer *w, const char *s);
/* shortest decimal that reads back as v; never an exponent; finite only */
MMD_API void mmd_w_num(mmd_writer *w, double v);
/* one sankey CSV field, quoted only when needed; mermaid's sankey takes printable ASCII only */
MMD_API void mmd_w_csv(mmd_writer *w, const char *s);
/* %%{init}%% before the diagram. key is dotted (xyChart.showDataLabel); value is a scalar
 * without quotes, backslashes or control bytes; true, false and JSON numbers are written bare.
 * Several calls are fine. */
MMD_API void mmd_w_config(mmd_writer *w, const char *key, const char *value);


/* ---- typed helpers: each writes whole lines ---- */
/* xychart; title may be nullptr */
MMD_API void mmd_xy_begin(mmd_writer *w, const char *title, bool horizontal);
MMD_API void mmd_xy_x_cats(mmd_writer *w, const char *title, size_t n, const char *const *cats);
/* lo == hi leaves the range automatic */
MMD_API void mmd_xy_x_range(mmd_writer *w, const char *title, double lo, double hi);
MMD_API void mmd_xy_y_range(mmd_writer *w, const char *title, double lo, double hi);
MMD_API void mmd_xy_bar(mmd_writer *w, const char *name, size_t n, const double *v);
MMD_API void mmd_xy_line(mmd_writer *w, const char *name, size_t n, const double *v);

MMD_API void mmd_pie_begin(mmd_writer *w, const char *title, bool show_data);
MMD_API void mmd_pie_slice(mmd_writer *w, const char *label, double v);

typedef enum mmd_shape : unsigned char {
    MMD_SHAPE_RECT,        /* [ ] */
    MMD_SHAPE_ROUND,       /* ( ) */
    MMD_SHAPE_STADIUM,     /* ([ ]) */
    MMD_SHAPE_SUBROUTINE,  /* [[ ]] */
    MMD_SHAPE_CYLINDER,    /* [( )] */
    MMD_SHAPE_CIRCLE,      /* (( )) */
    MMD_SHAPE_DCIRCLE,     /* ((( ))) */
    MMD_SHAPE_ASYM,        /* > ] */
    MMD_SHAPE_RHOMBUS,     /* { } */
    MMD_SHAPE_HEXAGON,     /* {{ }} */
    MMD_SHAPE_LEAN_R,      /* [/ /] */
    MMD_SHAPE_LEAN_L,      /* [\ \] */
    MMD_SHAPE_TRAP,        /* [/ \] */
    MMD_SHAPE_TRAP_ALT,    /* [\ /] */
} mmd_shape;
/* dir: TB, TD, BT, LR or RL */
MMD_API void mmd_flow_begin(mmd_writer *w, const char *dir);
/* label nullptr: the id is the label; '\n' breaks lines */
MMD_API void mmd_flow_node(mmd_writer *w, const char *id, const char *label, mmd_shape shape);
MMD_API void mmd_flow_edge(mmd_writer *w, const char *from, const char *to, const char *label);

/* Helpers below that open a block (sequence loop/alt/..., state group, ER entity) nest
 * the following lines until mmd_end(); text and render report blocks left open. */
MMD_API void mmd_end(mmd_writer *w);

/* sequenceDiagram. An id that starts with a keyword ("endpoint", "Andrew") or a digit
 * gets its first character written as an entity, so mermaid does not lex the keyword;
 * mention every participant through these helpers so the spelling matches. */
MMD_API void mmd_seq_begin(mmd_writer *w, const char *title, bool autonumber);
/* alias: the displayed name ('\n' breaks lines), or nullptr */
MMD_API void mmd_seq_participant(mmd_writer *w, const char *id, const char *alias, bool actor);
typedef enum mmd_arrow : unsigned char {
    MMD_ARROW_SOLID,        /* ->> */
    MMD_ARROW_DOTTED,       /* -->> */
    MMD_ARROW_LINE,         /* -> */
    MMD_ARROW_DOTTED_LINE,  /* --> */
    MMD_ARROW_CROSS,        /* -x */
    MMD_ARROW_DOTTED_CROSS, /* --x */
    MMD_ARROW_ASYNC,        /* -) */
    MMD_ARROW_DOTTED_ASYNC, /* --) */
} mmd_arrow;
MMD_API void mmd_seq_message(mmd_writer *w, const char *from, const char *to, mmd_arrow arrow, const char *text);
typedef enum mmd_side : unsigned char { MMD_LEFT_OF, MMD_RIGHT_OF, MMD_OVER } mmd_side;
/* b: a second participant for a note over both, else nullptr */
MMD_API void mmd_seq_note(mmd_writer *w, mmd_side side, const char *a, const char *b, const char *text);
MMD_API void mmd_seq_activate(mmd_writer *w, const char *id, bool on);
/* kw: loop, opt, break, alt, par or critical open a block; else (in alt), and (in par),
 * option (in critical) start its next part. label may be nullptr. */
MMD_API void mmd_seq_block(mmd_writer *w, const char *kw, const char *label);

/* gantt, dates as YYYY-MM-DD; title may be nullptr */
MMD_API void mmd_gantt_begin(mmd_writer *w, const char *title);
MMD_API void mmd_gantt_section(mmd_writer *w, const char *name);
enum { MMD_TASK_DONE = 1u, MMD_TASK_ACTIVE = 2u, MMD_TASK_CRIT = 4u, MMD_TASK_MILESTONE = 8u };
/* id may be nullptr; start is a date, the id of the task to follow, or nullptr (after the
 * previous task); days >= 0, fractions allowed. A name that would lex as a keyword or a
 * date ("section 2", "2026-05-01 launch") gets its first character as an entity. */
MMD_API void mmd_gantt_task(mmd_writer *w, const char *name, const char *id, unsigned flags, const char *start,
                            double days);

/* stateDiagram-v2; dir: nullptr, TB, BT, LR or RL. A state id nullptr is [*]. */
MMD_API void mmd_state_begin(mmd_writer *w, const char *dir);
/* label: the displayed name ('\n' breaks lines), or nullptr for the id */
MMD_API void mmd_state(mmd_writer *w, const char *id, const char *label);
/* a composite state: what follows, up to mmd_end(), is inside it */
MMD_API void mmd_state_group(mmd_writer *w, const char *id, const char *label);
MMD_API void mmd_state_transition(mmd_writer *w, const char *from, const char *to, const char *label);
/* side: MMD_LEFT_OF or MMD_RIGHT_OF */
MMD_API void mmd_state_note(mmd_writer *w, mmd_side side, const char *id, const char *text);

/* sankey-beta: value >= 0; names are printable ASCII (mermaid's CSV grammar) */
MMD_API void mmd_sankey_begin(mmd_writer *w);
MMD_API void mmd_sankey_flow(mmd_writer *w, const char *source, const char *target, double value);

/* mindmap and treeView-beta nest by depth: the first node is at 0, each next at most one
 * deeper than the one before. A mindmap has a single root. */
MMD_API void mmd_mindmap_begin(mmd_writer *w);
/* shape: RECT, ROUND, CIRCLE or HEXAGON; label nullptr with RECT writes the bare id */
MMD_API void mmd_mindmap_node(mmd_writer *w, unsigned depth, const char *id, const char *label, mmd_shape shape);
MMD_API void mmd_tree_begin(mmd_writer *w);
/* name: any text; dir: draw it as a folder; desc: a "## …" description, or nullptr */
MMD_API void mmd_tree_node(mmd_writer *w, unsigned depth, const char *name, bool dir, const char *desc);

/* erDiagram. Entity names are any text: they are always quoted, so names may hold
 * '-', blanks or keywords; alias (nullptr for none) is the displayed name. */
MMD_API void mmd_er_begin(mmd_writer *w);
/* opens the entity's attribute block: mmd_er_attr() lines, then mmd_end() */
MMD_API void mmd_er_entity(mmd_writer *w, const char *name, const char *alias);
enum { MMD_KEY_PK = 1u, MMD_KEY_FK = 2u, MMD_KEY_UK = 4u };
/* type, name: [A-Za-z_*] then [A-Za-z0-9_*.,()[]-] (non-ASCII too), e.g. varchar(255), tags[];
 * comment may be nullptr */
MMD_API void mmd_er_attr(mmd_writer *w, const char *type, const char *name, unsigned keys, const char *comment);
typedef enum mmd_card : unsigned char {
    MMD_CARD_ONE,          /* || */
    MMD_CARD_ZERO_OR_ONE,  /* |o  o| */
    MMD_CARD_ONE_OR_MORE,  /* }|  |{ */
    MMD_CARD_ZERO_OR_MORE, /* }o  o{ */
} mmd_card;
/* identifying: -- (else ..); label may be nullptr */
MMD_API void mmd_er_rel(mmd_writer *w, const char *a, mmd_card ca, const char *b, mmd_card cb, bool identifying,
                        const char *label);

/* gitGraph; dir: nullptr, LR, TB or BT. Branch names: \w([-./\w]*[-\w])?, not a keyword
 * (commit, merge, ...) or a number. Commit ids and tags may be nullptr. */
typedef enum mmd_commit : unsigned char { MMD_COMMIT_NORMAL, MMD_COMMIT_REVERSE, MMD_COMMIT_HIGHLIGHT } mmd_commit;
MMD_API void mmd_git_begin(mmd_writer *w, const char *dir);
MMD_API void mmd_git_commit(mmd_writer *w, const char *id, const char *tag, mmd_commit type);
MMD_API void mmd_git_branch(mmd_writer *w, const char *name);
MMD_API void mmd_git_checkout(mmd_writer *w, const char *name);
MMD_API void mmd_git_merge(mmd_writer *w, const char *name, const char *id, const char *tag, mmd_commit type);
MMD_API void mmd_git_cherry_pick(mmd_writer *w, const char *id);

#ifdef __cplusplus
}
#endif
#endif
