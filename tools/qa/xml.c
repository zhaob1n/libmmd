/* A small XML reader: enough to say an SVG is well-formed and to walk it. No DTD, no namespaces
 * beyond keeping prefixes in names. */
#include "qa.h"

#include <setjmp.h>
#include <strings.h>

struct xml_block {
    struct xml_block *next;
    size_t used, cap;
    char data[];
};

typedef struct parser {
    const char *src, *p, *end;
    xml_doc *doc;
    qa_buf tmp; /* decoded text on its way into the arena */
    const char *why;
    jmp_buf fail;
} parser;

[[noreturn]] static void fail(parser P[static 1], const char *why) {
    P->why = why;
    longjmp(P->fail, 1);
}

static void *alloc(parser P[static 1], size_t size) {
    size = (size + 7) & ~(size_t)7;
    struct xml_block *b = P->doc->blocks;
    if (!b || b->cap - b->used < size) {
        size_t cap = size > 1 << 16 ? size : 1 << 16;
        b = qa_alloc(sizeof *b + cap);
        b->cap = cap;
        b->next = P->doc->blocks;
        P->doc->blocks = b;
    }
    b->used += size;
    return b->data + b->used - size;
}

static const char *keep(parser P[static 1], size_t n, const char s[n]) {
    char *d = alloc(P, n + 1);
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static bool xml_char(uint32_t c) {
    return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) ||
           (c >= 0x10000 && c <= 0x10FFFF);
}

/* the whole source is legal characters in shortest-form UTF-8; the parser then works on bytes */
static void check_chars(parser P[static 1]) {
    for (P->p = P->src; P->p < P->end;) {
        unsigned char c = (unsigned char)*P->p;
        unsigned len = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : 0;
        if (!len || (size_t)(P->end - P->p) < len) fail(P, "invalid UTF-8");
        uint32_t cp = len == 1 ? c : c & (0x7Fu >> len);
        for (unsigned i = 1; i < len; i++) {
            unsigned char t = (unsigned char)P->p[i];
            if (t >> 6 != 2) fail(P, "invalid UTF-8");
            cp = cp << 6 | (t & 0x3F);
        }
        static const uint32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < least[len]) fail(P, "overlong UTF-8");
        if (!xml_char(cp)) fail(P, "character XML does not allow");
        P->p += len;
    }
}

static bool at(const parser P[static 1], const char *lit) {
    size_t n = strlen(lit);
    return (size_t)(P->end - P->p) >= n && memcmp(P->p, lit, n) == 0;
}

static bool eat(parser P[static 1], const char *lit) {
    if (!at(P, lit)) return false;
    P->p += strlen(lit);
    return true;
}

static bool space(parser P[static 1]) {
    const char *p = P->p;
    while (P->p < P->end && (*P->p == ' ' || *P->p == '\t' || *P->p == '\n' || *P->p == '\r')) P->p++;
    return P->p != p;
}

static bool name_byte(char ch, bool first) {
    unsigned char c = (unsigned char)ch;
    if (c >= 0x80 || c == '_' || c == ':' || (unsigned)((c | 32) - 'a') < 26) return true;
    return !first && ((unsigned)(c - '0') < 10 || c == '-' || c == '.');
}

static const char *name(parser P[static 1]) {
    const char *p = P->p;
    if (p == P->end || !name_byte(*p, true)) fail(P, "name expected");
    while (P->p < P->end && name_byte(*P->p, false)) P->p++;
    return keep(P, (size_t)(P->p - p), p);
}

/* past the terminator, wherever it is */
static void until(parser P[static 1], const char *term, const char *why) {
    const char *e = memmem(P->p, (size_t)(P->end - P->p), term, strlen(term));
    if (!e) fail(P, why);
    P->p = e + strlen(term);
}

static void comment(parser P[static 1]) {
    until(P, "--", "unclosed comment");
    if (!eat(P, ">")) fail(P, "-- inside a comment");
}

static void pi(parser P[static 1]) {
    const char *target = name(P);
    if (strcasecmp(target, "xml") == 0) fail(P, "XML declaration after the start");
    until(P, "?>", "unclosed processing instruction");
}

/* &...; after the '&', decoded into tmp */
static void reference(parser P[static 1]) {
    static const struct { const char *name; char c; } named[] = {{"lt;", '<'}, {"gt;", '>'}, {"amp;", '&'}, {"quot;", '"'}, {"apos;", '\''}};
    for (size_t i = 0; i < countof(named); i++) {
        if (eat(P, named[i].name)) {
            qa_put(&P->tmp, 1, &named[i].c);
            return;
        }
    }
    if (!eat(P, "#")) fail(P, "undefined entity");
    unsigned base = eat(P, "x") ? 16 : 10;
    uint32_t cp = 0;
    const char *digits = P->p;
    for (; P->p < P->end && *P->p != ';'; P->p++) {
        unsigned char c = (unsigned char)*P->p;
        unsigned d = (unsigned)(c - '0') < 10 ? c - '0' : base == 16 && (unsigned)((c | 32) - 'a') < 6 ? (c | 32) - 'a' + 10 : 99;
        if (d == 99 || cp > 0x10FFFF) fail(P, "bad character reference");
        cp = cp * base + d;
    }
    if (P->p == digits || !eat(P, ";") || !xml_char(cp)) fail(P, "bad character reference");
    char u[4];
    size_t n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (n == 1) u[0] = (char)cp;
    else {
        for (size_t i = n - 1; i; i--, cp >>= 6) u[i] = (char)(0x80 | (cp & 0x3F));
        u[0] = (char)((0xF00 >> n) | cp);
    }
    qa_put(&P->tmp, n, u);
}

static xml_node *node(parser P[static 1], xml_node *parent, xml_node **last) {
    xml_node *n = alloc(P, sizeof *n);
    *n = (xml_node){.parent = parent};
    if (*last) (*last)->next = n;
    else if (parent) parent->child = n;
    *last = n;
    return n;
}

static void text(parser P[static 1], xml_node *parent, xml_node **last) {
    node(P, parent, last)->text = keep(P, P->tmp.n, P->tmp.p ? P->tmp.p : "");
}

static void attributes(parser P[static 1], xml_node e[static 1]) {
    size_t cap = 0;
    for (;;) {
        bool sp = space(P);
        if (at(P, ">") || at(P, "/>")) return;
        if (!sp) fail(P, "space expected before an attribute");
        const char *an = name(P);
        for (size_t i = 0; i < e->nattrs; i++)
            if (strcmp(e->attrs[i].name, an) == 0) fail(P, "attribute given twice");
        space(P);
        if (!eat(P, "=")) fail(P, "= expected after an attribute name");
        space(P);
        if (P->p == P->end || (*P->p != '"' && *P->p != '\'')) fail(P, "quoted attribute value expected");
        char q = *P->p++;
        P->tmp.n = 0;
        for (;; P->p++) {
            if (P->p == P->end) fail(P, "unclosed attribute value");
            if (*P->p == q) break;
            if (*P->p == '<') fail(P, "< in an attribute value");
            if (*P->p != '&') qa_put(&P->tmp, 1, P->p);
            else {
                P->p++;
                reference(P);
                P->p--;
            }
        }
        P->p++;
        if (e->nattrs == cap) { /* the arena only grows: double, copy */
            xml_attr *v = alloc(P, (cap = cap ? 2 * cap : 8) * sizeof *v);
            if (e->nattrs) memcpy(v, e->attrs, e->nattrs * sizeof *v);
            e->attrs = v;
        }
        e->attrs[e->nattrs++] = (xml_attr){an, keep(P, P->tmp.n, P->tmp.p ? P->tmp.p : "")};
    }
}

/* at the name after '<' */
static xml_node *element(parser P[static 1], xml_node *parent, xml_node **sibling, unsigned depth) {
    if (depth > 2000) fail(P, "elements nested too deep");
    xml_node *e = node(P, parent, sibling), *last = nullptr;
    e->name = name(P);
    attributes(P, e);
    if (eat(P, "/>")) return e;
    P->p++;
    for (;;) {
        if (P->p == P->end) fail(P, "unclosed element");
        if (eat(P, "</")) {
            const char *p = P->p;
            if (!eat(P, e->name) || (P->p < P->end && name_byte(*P->p, false))) {
                P->p = p;
                fail(P, "end tag does not match its start tag");
            }
            space(P);
            if (!eat(P, ">")) fail(P, "> expected");
            return e;
        }
        if (eat(P, "<!--")) comment(P);
        else if (eat(P, "<?")) pi(P);
        else if (eat(P, "<![CDATA[")) {
            const char *p = P->p;
            until(P, "]]>", "unclosed CDATA section");
            P->tmp.n = 0;
            qa_put(&P->tmp, (size_t)(P->p - 3 - p), p);
            text(P, e, &last);
        } else if (eat(P, "<")) element(P, e, &last, depth + 1);
        else {
            P->tmp.n = 0;
            for (; P->p < P->end && *P->p != '<'; P->p++) {
                if (at(P, "]]>")) fail(P, "]]> in text");
                if (*P->p != '&') qa_put(&P->tmp, 1, P->p);
                else {
                    P->p++;
                    reference(P);
                    P->p--;
                }
            }
            text(P, e, &last);
        }
    }
}

static void misc(parser P[static 1]) {
    for (;;) {
        space(P);
        if (eat(P, "<!--")) comment(P);
        else if (eat(P, "<?")) pi(P);
        else return;
    }
}

xml_doc *xml_parse(size_t n, const char src[static n], char err[static 128]) {
    /* on the heap: what longjmp() leaves of a changed automatic object is indeterminate */
    parser *const P = qa_alloc(sizeof *P);
    *P = (parser){.src = src, .p = src, .end = src + n, .doc = qa_alloc(sizeof(xml_doc))};
    xml_doc *doc = P->doc;
    if (setjmp(P->fail)) {
        unsigned line = 1, col = 1;
        for (const char *p = src; p < P->p; p++) {
            if (*p == '\n') { line++; col = 1; }
            else col++;
        }
        snprintf(err, 128, "%u:%u: %s", line, col, P->why);
        xml_free(doc);
        doc = nullptr;
    } else {
        check_chars(P);
        P->p = src;
        if (at(P, "<?xml") && n > 5 && !name_byte(src[5], false)) until(P, "?>", "unclosed XML declaration");
        misc(P);
        if (at(P, "<!DOCTYPE")) fail(P, "DOCTYPE is not supported");
        if (!eat(P, "<")) fail(P, "root element expected");
        xml_node *last = nullptr;
        doc->root = element(P, nullptr, &last, 0);
        misc(P);
        if (P->p != P->end) fail(P, "content after the root element");
    }
    free(P->tmp.p);
    free(P);
    return doc;
}

void xml_free(xml_doc *d) {
    if (!d) return;
    for (struct xml_block *b = d->blocks, *next; b; b = next) {
        next = b->next;
        free(b);
    }
    free(d);
}

const char *xml_get(const xml_node e[static 1], const char *name) {
    for (size_t i = 0; i < e->nattrs; i++)
        if (strcmp(e->attrs[i].name, name) == 0) return e->attrs[i].value;
    return nullptr;
}

bool xml_is(const xml_node *e, const char *name) { return e && e->name && strcmp(e->name, name) == 0; }

bool xml_has_class(const xml_node e[static 1], const char *cls) {
    const char *v = e->name ? xml_get(e, "class") : nullptr;
    size_t n = strlen(cls);
    for (const char *p = v; p && (p = strstr(p, cls)); p += n)
        if ((p == v || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return true;
    return false;
}

xml_node *xml_skip(const xml_node top[static 1], const xml_node n[static 1]) {
    for (; n != top; n = n->parent)
        if (n->next) return n->next;
    return nullptr;
}

xml_node *xml_next(const xml_node top[static 1], const xml_node n[static 1]) {
    return n->child ? n->child : xml_skip(top, n);
}

void xml_text(const xml_node e[static 1], const char *sep, qa_buf out[static 1]) {
    bool any = false;
    for (const xml_node *n = e; n; n = xml_next(e, n)) {
        if (!n->text) continue;
        const char *s = n->text + strspn(n->text, " \t\n\r"), *end = s + strlen(s);
        while (end > s && strchr(" \t\n\r", end[-1])) end--;
        if (end == s) continue;
        if (any) qa_puts(out, sep);
        qa_put(out, (size_t)(end - s), s);
        any = true;
    }
}
