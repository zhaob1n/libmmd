/* libmmd — Mermaid source writer. The text it builds is the only interface:
 * mmd_writer_text() hands it out (to save as .mmd), mmd_writer_render_svg()
 * renders that same text, so a saved file re-renders byte-identically.
 *
 * Errors are sticky, like ferror(): no call needs checking. A value a
 * context cannot carry (a quote in an identifier, a newline where the
 * diagram draws no line breaks, NaN, invalid UTF-8, ...) makes every later
 * call a no-op, and the text/render call reports it. Strings are UTF-8 and
 * NUL-terminated; a nullptr string where one is optional means "none".
 * Escaping and the per-diagram rules behind it: docs/dev/writer.md. */
#ifndef MMD_WRITER_H
#define MMD_WRITER_H

#include "mmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmd_writer mmd_writer;

[[nodiscard]] MMD_API mmd_writer *mmd_writer_new(void);
MMD_API void mmd_writer_free(mmd_writer *w);
/* empty the text and clear the error; keeps the memory */
MMD_API void mmd_writer_reset(mmd_writer *w);

/* *s (NUL-terminated, *len bytes) stays valid until the next call on w.
 * On error *s is nullptr and err (nullable) says which call failed and where
 * in the text (line/col) its value would have gone. */
[[nodiscard]] MMD_API mmd_status mmd_writer_text(const mmd_writer *w, const char **s, size_t *len, mmd_error *err);
/* mmd_render_svg() of exactly that text */
[[nodiscard]] MMD_API mmd_status mmd_writer_render_svg(const mmd_writer *w, const mmd_options *opt,
                                                       mmd_buffer *out, mmd_error *err);

/* ---- escape contexts: work in every diagram type ---- */
enum {
    MMD_W_BR = 1u, /* '\n' becomes <br>; only where the diagram draws line breaks */
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
/* node / participant / entity id: [A-Za-z0-9_] and non-ASCII, non-empty */
MMD_API void mmd_w_id(mmd_writer *w, const char *s);
/* shortest decimal that reads back as v; never an exponent; finite only */
MMD_API void mmd_w_num(mmd_writer *w, double v);
/* one sankey CSV field, quoted only when needed */
MMD_API void mmd_w_csv(mmd_writer *w, const char *s);

#ifdef __cplusplus
}
#endif
#endif
