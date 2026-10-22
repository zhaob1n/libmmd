#include "base.h"

#include <stdckdint.h>
#include <stdlib.h>

constexpr size_t ARENA_MIN_BLOCK = 64 * 1024;

void *arena_alloc(Arena *a, size_t size, size_t align) {
    ArenaBlock *b = a->head;
    if (b) {
        size_t off = (b->used + (align - 1)) & ~(align - 1);
        if (off <= b->cap && b->cap - off >= size) {
            b->used = off + size;
            a->last = b->data + off;
            a->last_size = size;
            return a->last;
        }
    }
    size_t cap = ARENA_MIN_BLOCK;
    if (b && b->cap < (size_t)64 << 20) cap = b->cap * 2;
    size_t need = 0;
    if (ckd_add(&need, size, align)) return nullptr;
    if (cap < need) cap = need;
    size_t total = 0;
    if (ckd_add(&total, cap, sizeof(ArenaBlock))) return nullptr;
    ArenaBlock *nb = malloc(total);
    if (!nb) return nullptr;
    nb->next = b;
    nb->cap = cap;
    nb->used = size;
    a->head = nb;
    a->last = nb->data;
    a->last_size = size;
    return nb->data;
}

void *arena_alloc_array(Arena *a, size_t n, size_t size, size_t align) {
    size_t total = 0;
    if (ckd_mul(&total, n, size)) return nullptr;
    void *p = arena_alloc(a, total ? total : 1, align);
    return p;
}

void *arena_grow(Arena *a, void *old, size_t *cap, size_t elem, size_t align) {
    size_t ncap = *cap ? *cap * 2 : 8;
    size_t oldb = *cap * elem, newb = 0;
    if (ckd_mul(&newb, ncap, elem)) return nullptr;
    ArenaBlock *b = a->head;
    /* in-place growth if old is the most recent allocation */
    if (old && old == a->last && b) {
        unsigned char *op = old;
        size_t off = (size_t)(op - b->data);
        if (b->cap - off >= newb) {
            b->used = off + newb;
            a->last_size = newb;
            *cap = ncap;
            return old;
        }
    }
    void *np = arena_alloc(a, newb, align);
    if (!np) return nullptr;
    if (old && oldb) memcpy(np, old, oldb);
    *cap = ncap;
    return np;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *nx = b->next;
        free(b);
        b = nx;
    }
    *a = (Arena){};
}

void arena_reset(Arena *a) {
    ArenaBlock *b = a->head;
    if (!b) return;
    size_t total = 0;
    unsigned nb = 0;
    for (ArenaBlock *c = b; c; c = c->next) total += c->cap, nb++;
    if (nb > 1) {
        arena_free(a);
        ArenaBlock *n = malloc(sizeof(ArenaBlock) + total);
        if (!n) return; /* fall back to lazy allocation */
        *n = (ArenaBlock){.cap = total};
        a->head = n;
    } else {
        b->used = 0;
    }
    a->last = nullptr;
    a->last_size = 0;
}

Str arena_strdup(Arena *a, Str s) {
    char *p = arena_alloc(a, s.n + 1, 1);
    if (!p) return (Str){};
    if (s.n) memcpy(p, s.p, s.n);
    p[s.n] = 0;
    return (Str){.p = p, .n = s.n};
}

bool buf_reserve_slow(Buf *b, size_t extra) {
    if (b->oom) return false;
    size_t need = 0;
    if (ckd_add(&need, b->n, extra + 1)) goto fail;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < need) {
        if (ckd_mul(&ncap, ncap, 2)) goto fail;
    }
    char *np = realloc(b->p, ncap);
    if (!np) goto fail;
    b->p = np;
    b->cap = ncap;
    return true;
fail:
    b->oom = true;
    return false;
}

static const char DIGITS2[201] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

static inline size_t u64_to_str(uint64_t v, char out[static 24]) {
    char tmp[24];
    size_t i = sizeof tmp;
    while (v >= 100) {
        unsigned d = (unsigned)(v % 100);
        v /= 100;
        tmp[--i] = DIGITS2[d * 2 + 1];
        tmp[--i] = DIGITS2[d * 2];
    }
    if (v >= 10) {
        tmp[--i] = DIGITS2[v * 2 + 1];
        tmp[--i] = DIGITS2[v * 2];
    } else {
        tmp[--i] = (char)('0' + v);
    }
    size_t len = sizeof tmp - i;
    memcpy(out, tmp + i, len);
    return len;
}

void buf_int(Buf *b, int64_t v) {
    if (!buf_reserve(b, 24)) return;
    char *o = b->p + b->n;
    uint64_t u = (uint64_t)v;
    if (v < 0) {
        *o++ = '-';
        u = 0 - u;
        b->n++;
    }
    b->n += u64_to_str(u, o);
}

void buf_num(Buf *b, double v) {
    if (!(v == v) || v > 1e15 || v < -1e15) v = 0; /* NaN / absurd guard */
    int64_t c = (int64_t)(v * 100.0 + (v < 0 ? -0.5 : 0.5));
    if (!buf_reserve(b, 26)) return;
    char *o = b->p + b->n;
    size_t k = 0;
    uint64_t u = (uint64_t)c;
    if (c < 0) {
        o[k++] = '-';
        u = 0 - u;
    }
    uint64_t ip = u / 100;
    unsigned fp = (unsigned)(u % 100);
    k += u64_to_str(ip, o + k);
    if (fp) {
        o[k++] = '.';
        o[k++] = (char)('0' + fp / 10);
        if (fp % 10) o[k++] = (char)('0' + fp % 10);
    } else if (c < 0 && ip == 0) {
        k = 0;
        o[k++] = '0';
    }
    b->n += k;
}

void buf_xml(Buf *b, Str s) {
    size_t i = 0, run = 0;
    while (i < s.n) {
        i = scan_xml_plain(s.n, s.p, i); /* bulk-skip bytes needing no escape */
        if (i >= s.n) break;
        char c = s.p[i];
        const char *rep = nullptr;
        size_t rl = 0;
        switch (c) {
        case '<': rep = "&lt;"; rl = 4; break;
        case '>': rep = "&gt;"; rl = 4; break;
        case '&': rep = "&amp;"; rl = 5; break;
        case '"': rep = "&quot;"; rl = 6; break;
        case '#': {
            /* mermaid entity: #name; or #123; */
            size_t j = i + 1;
            while (j < s.n && j - i < 12 &&
                   ((unsigned)((s.p[j] | 32) - 'a') < 26u || (unsigned)(s.p[j] - '0') < 10u)) j++;
            if (j < s.n && s.p[j] == ';' && j > i + 1) {
                buf_put(b, i - run, s.p + run);
                buf_ch(b, '&');
                if ((unsigned)(s.p[i + 1] - '0') < 10u) buf_ch(b, '#');
                buf_put(b, j - i, s.p + i + 1);
                i = j + 1;
                run = i;
                continue;
            }
            break;
        }
        default:
            if ((unsigned char)c < 0x20 && c != '\t' && c != '\n') { rep = " "; rl = 1; }
            break;
        }
        if (rep) {
            buf_put(b, i - run, s.p + run);
            buf_put(b, rl, rep);
            run = i + 1;
        }
        i++;
    }
    buf_put(b, s.n - run, s.p + run);
}
