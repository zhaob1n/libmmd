#ifndef CMM_BASE_H
#define CMM_BASE_H
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define CMM_LIKELY(x) __builtin_expect(!!(x), 1)
#define CMM_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ---- zero-copy string view ---- */
typedef struct Str {
    const char *p;
    size_t n;
} Str;

#define S(lit) ((Str){.p = (lit), .n = sizeof(lit) - 1})

static inline bool str_eq(Str a, Str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static inline bool str_ieq(Str a, Str b) {
    if (a.n != b.n) return false;
    for (size_t i = 0; i < a.n; i++) {
        unsigned char x = (unsigned char)a.p[i], y = (unsigned char)b.p[i];
        if ((unsigned)(x - 'A') < 26u) x += 32;
        if ((unsigned)(y - 'A') < 26u) y += 32;
        if (x != y) return false;
    }
    return true;
}
static inline bool is_sp(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static inline Str str_trim(Str s) {
    while (s.n && (is_sp(s.p[0]) || s.p[0] == '\n')) s.p++, s.n--;
    while (s.n && (is_sp(s.p[s.n - 1]) || s.p[s.n - 1] == '\n')) s.n--;
    return s;
}
static inline bool str_starts(Str s, Str pre) { return s.n >= pre.n && memcmp(s.p, pre.p, pre.n) == 0; }

/* ---- arena ---- */
typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock {
    ArenaBlock *next;
    size_t cap, used;
    alignas(max_align_t) unsigned char data[];
};
typedef struct Arena {
    ArenaBlock *head;
    void *last; /* last allocation (for in-place growth) */
    size_t last_size;
} Arena;

[[nodiscard]] void *arena_alloc(Arena *a, size_t size, size_t align);
[[nodiscard]] void *arena_alloc_array(Arena *a, size_t n, size_t size, size_t align);
/* grow an array previously allocated from the arena (or nullptr). */
[[nodiscard]] void *arena_grow(Arena *a, void *old, size_t *cap, size_t elem, size_t align);
void arena_free(Arena *a);
[[nodiscard]] Str arena_strdup(Arena *a, Str s);

#define ANEW(a, T, n) arena_alloc_array((a), (n), sizeof(T), alignof(T))

/* typed growable vector living in an arena */
#define VEC(T) struct { T *v; size_t n, cap; }
#define VEC_PUSH(A, V, ...)                                                                       \
    ({                                                                                            \
        bool ok_ = true;                                                                          \
        if ((V)->n == (V)->cap) {                                                                 \
            typeof((V)->v) nv_ = arena_grow((A), (V)->v, &(V)->cap, sizeof *(V)->v,               \
                                            alignof(typeof(*(V)->v)));                            \
            if (nv_) (V)->v = nv_; else ok_ = false;                                              \
        }                                                                                         \
        if (ok_) (V)->v[(V)->n++] = (__VA_ARGS__);                                                \
        ok_;                                                                                      \
    })

/* ---- output buffer (malloc-backed, handed to user) ---- */
typedef struct Buf {
    char *p;
    size_t n, cap;
    bool oom;
} Buf;

bool buf_reserve_slow(Buf *b, size_t extra);
static inline bool buf_reserve(Buf *b, size_t extra) {
    if (CMM_LIKELY(b->cap - b->n > extra)) return true;
    return buf_reserve_slow(b, extra);
}
static inline void buf_put_(Buf *b, size_t n, const char s[static n]) {
    if (!buf_reserve(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
/* zero-length appends are common (escape runs); [static 0] would be UB, so filter here */
#define buf_put(b, n, s) do { size_t n_ = (n); if (n_) buf_put_((b), n_, (s)); } while (0)
static inline void buf_str(Buf *b, Str s) { if (s.n) buf_put(b, s.n, s.p); }
#define buf_lit(b, lit) buf_put((b), sizeof(lit) - 1, (lit))
static inline void buf_ch(Buf *b, char c) {
    if (!buf_reserve(b, 1)) return;
    b->p[b->n++] = c;
}
void buf_num(Buf *b, double v);  /* fast fixed 2-decimal, trimmed */
void buf_int(Buf *b, int64_t v);
void buf_xml(Buf *b, Str s);     /* XML-escape text, handles mermaid #entity; */

#endif
