/* libmmd — fast Mermaid renderer in modern C. Public API. */
#ifndef MMD_H
#define MMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MMD_API __attribute__((visibility("default")))
#else
#define MMD_API
#endif

typedef enum mmd_status {
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
[[nodiscard]] MMD_API mmd_status mmd_render_svg(size_t n, const char src[static n],
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
[[nodiscard]] MMD_API mmd_status mmd_context_render_svg(mmd_context *ctx, size_t n, const char src[static n],
                                                const mmd_options *opt, const char **svg, size_t *len,
                                                mmd_error *err);

[[nodiscard]] MMD_API const char *mmd_status_str(mmd_status s);

#ifdef __cplusplus
}
#endif
#endif
