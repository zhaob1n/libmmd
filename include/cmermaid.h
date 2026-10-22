/* cmermaid — fast Mermaid renderer in modern C. Public API. */
#ifndef CMERMAID_H
#define CMERMAID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
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

typedef struct cmm_options {
    double node_spacing; /* 0 => default (50) */
    double rank_spacing; /* 0 => default (50) */
    double font_size;    /* 0 => default (16) */
} cmm_options;

/* Render Mermaid source (n bytes, need not be NUL-terminated) to SVG.
 * opt may be nullptr. err may be nullptr. On success *out owns a buffer. */
[[nodiscard]] cmm_status cmm_render_svg(size_t n, const char src[static n],
                                        const cmm_options *opt,
                                        cmm_buffer *out, cmm_error *err);

void cmm_buffer_free(cmm_buffer *buf);

[[nodiscard]] const char *cmm_status_str(cmm_status s);

#ifdef __cplusplus
}
#endif
#endif
