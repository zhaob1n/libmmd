#ifndef MMD_SEQ_H
#define MMD_SEQ_H
#include "base.h"
#include "mmd.h"
[[nodiscard]] mmd_status seq_render(Arena *a, size_t n, const char src[static n], size_t off, Str title,
                                    double fs, Buf *out, size_t *err_off, char msg[static 160]);
#endif
