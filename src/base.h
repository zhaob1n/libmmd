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
/* ---- 16-bytes-at-a-time scanners (SSE2; scalar tail / fallback) ----
 * Each returns the first index >= i (and < n) whose byte is NOT in the class, or n. */
#if defined(__SSE2__)
#include <emmintrin.h>
#define SCAN16(cond_vec, cond_scalar)                                                       \
    for (; i + 16 <= n; i += 16) {                                                          \
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));                \
        unsigned m = (unsigned)_mm_movemask_epi8(cond_vec) ^ 0xFFFFu;                       \
        if (m) return i + (size_t)__builtin_ctz(m);                                          \
    }                                                                                        \
    for (; i < n && (cond_scalar); i++) {}                                                   \
    return i;
/* identifier bytes: [0-9A-Za-z_] and >= 0x80 */
static inline size_t scan_id(size_t n, const char s[static n], size_t i) {
    #define C_ (unsigned char)s[i]
    SCAN16(_mm_or_si128(
               _mm_or_si128(_mm_cmplt_epi8(v, _mm_setzero_si128()), /* >= 0x80 */
                            _mm_cmpeq_epi8(v, _mm_set1_epi8('_'))),
               _mm_or_si128(
                   _mm_and_si128(_mm_cmpgt_epi8(v, _mm_set1_epi8('0' - 1)), _mm_cmplt_epi8(v, _mm_set1_epi8('9' + 1))),
                   _mm_and_si128(_mm_cmpgt_epi8(_mm_or_si128(v, _mm_set1_epi8(32)), _mm_set1_epi8('a' - 1)),
                                 _mm_cmplt_epi8(_mm_or_si128(v, _mm_set1_epi8(32)), _mm_set1_epi8('z' + 1))))),
           (C_ >= 0x80 || C_ == '_' || (unsigned)(C_ - '0') < 10u || (unsigned)((C_ | 32) - 'a') < 26u))
    #undef C_
}
/* blanks: ' ' '\t' '\r' */
static inline size_t scan_sp(size_t n, const char s[static n], size_t i) {
    if (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return i; /* common: no blank at all */
    SCAN16(_mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(' ')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\t'))),
                        _mm_cmpeq_epi8(v, _mm_set1_epi8('\r'))),
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'))
}
/* XML-plain bytes: not < > & " # and not a control char other than \t \n */
static inline size_t scan_xml_plain(size_t n, const char s[static n], size_t i) {
    SCAN16(_mm_andnot_si128(
               _mm_or_si128(_mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('<')), _mm_cmpeq_epi8(v, _mm_set1_epi8('>'))),
                                         _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('&')), _mm_cmpeq_epi8(v, _mm_set1_epi8('"')))),
                            _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('#')),
                                         _mm_and_si128(_mm_cmpgt_epi8(v, _mm_set1_epi8(-1)), _mm_cmplt_epi8(v, _mm_set1_epi8(0x20))))),
               _mm_set1_epi8(-1)),
           !(s[i] == '<' || s[i] == '>' || s[i] == '&' || s[i] == '"' || s[i] == '#' || (unsigned char)s[i] < 0x20))
}
#undef SCAN16
#else
static inline size_t scan_id(size_t n, const char s[static n], size_t i) {
    for (; i < n; i++) { unsigned char c = (unsigned char)s[i]; if (!(c >= 0x80 || c == '_' || (unsigned)(c - '0') < 10u || (unsigned)((c | 32) - 'a') < 26u)) break; }
    return i;
}
static inline size_t scan_sp(size_t n, const char s[static n], size_t i) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    return i;
}
static inline size_t scan_xml_plain(size_t n, const char s[static n], size_t i) {
    for (; i < n; i++) { char c = s[i]; if (c == '<' || c == '>' || c == '&' || c == '"' || c == '#' || (unsigned char)c < 0x20) break; }
    return i;
}
#endif

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
/* Drop all allocations but keep (coalesced) memory for reuse. */
void arena_reset(Arena *a);
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
void buf_pt(Buf *b, double x, double y); /* "x,y" with buf_num formatting, one reserve */
void buf_int(Buf *b, int64_t v);
void buf_xml(Buf *b, Str s);     /* XML-escape text, handles mermaid #entity; */

#endif
