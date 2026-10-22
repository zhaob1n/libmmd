#include "seq.h"
cmm_status seq_render(Arena *a, size_t n, const char src[static n], size_t off, Str title, double fs,
                      Buf *out, size_t *err_off, char msg[static 160]) {
    (void)a; (void)src; (void)off; (void)title; (void)fs; (void)out; (void)msg;
    *err_off = 0;
    return CMM_ERR_UNSUPPORTED;
}
