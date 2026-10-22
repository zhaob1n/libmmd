/* cmermaid — fast Mermaid renderer in modern C. Public API. */
#ifndef CMERMAID_H
#define CMERMAID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CMM_API __attribute__((visibility("default")))
#else
#define CMM_API
#endif

typedef enum cmm_status {
    CMM_OK = 0,
    CMM_ERR_SYNTAX,      /* parse error, see cmm_error line/col */
    CMM_ERR_UNSUPPORTED, /* diagram type not implemented */
    CMM_ERR_NOMEM,
    CMM_ERR_EMPTY,       /* no diagram found */
    CMM_ERR_LIMIT,       /* input exceeds internal limits */
} cmm_status;

typedef struct cmm_error {
    cmm_status code;
    uint32_t line; /* 1-based, 0 if unknown */
    uint32_t col;  /* 1-based, byte column */
    char message[160];
} cmm_error;

typedef struct cmm_buffer {
    char *data; /* NUL-terminated, owned; free with cmm_buffer_free */
    size_t len;
} cmm_buffer;

typedef enum cmm_theme {
    CMM_THEME_AUTO = 0, /* from %%{init}%% / frontmatter, else default */
    CMM_THEME_DEFAULT, CMM_THEME_DARK, CMM_THEME_FOREST, CMM_THEME_NEUTRAL,
} cmm_theme;

enum {
    CMM_F_ROUTE_AVOID = 1u, /* (default since v3; kept for compatibility) */
    CMM_F_NO_AVOID = 2u,    /* flowchart: disable re-routing edges around non-incident nodes */
};

typedef struct cmm_options {
    double node_spacing; /* 0 => default (50) */
    double rank_spacing; /* 0 => default (50) */
    double font_size;    /* 0 => default (16) */
    cmm_theme theme;     /* 0 => from source / default */
    uint32_t flags;      /* CMM_F_* */
} cmm_options;

/* Render Mermaid source (n bytes, need not be NUL-terminated) to SVG.
 * opt may be nullptr. err may be nullptr. On success *out owns a buffer. */
[[nodiscard]] CMM_API cmm_status cmm_render_svg(size_t n, const char src[static n],
                                        const cmm_options *opt,
                                        cmm_buffer *out, cmm_error *err);

CMM_API void cmm_buffer_free(cmm_buffer *buf);

/* Reusable render context for batch rendering: keeps its arena and output
 * buffer between calls so steady-state renders do (almost) no malloc. */
typedef struct cmm_context cmm_context;
[[nodiscard]] CMM_API cmm_context *cmm_context_new(void);
CMM_API void cmm_context_free(cmm_context *ctx);
/* On success *svg (NUL-terminated, *len bytes) is owned by ctx and stays
 * valid until the next call on ctx or cmm_context_free. Not thread-safe per
 * ctx; use one context per thread. */
[[nodiscard]] CMM_API cmm_status cmm_context_render_svg(cmm_context *ctx, size_t n, const char src[static n],
                                                const cmm_options *opt, const char **svg, size_t *len,
                                                cmm_error *err);

[[nodiscard]] CMM_API const char *cmm_status_str(cmm_status s);

#ifdef __cplusplus
}
#endif
#endif
