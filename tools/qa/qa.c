#define _GNU_SOURCE
#include "qa.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbit.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* These programs exit without freeing what they hold. Only they link this file, so the library and
 * the CLI stay under LeakSanitizer. The runtime looks the symbol up by name. */
[[gnu::visibility("default"), gnu::used]] const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "detect_leaks=0"; }

void qa_die(const char *fmt, ...) {
    int e = errno;
    fflush(stdout);
    fprintf(stderr, "%s: ", program_invocation_short_name);
    va_list ap;
    va_start(ap, fmt);
    errno = e; /* for %m */
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

void *qa_alloc(size_t size) {
    void *p = calloc(1, size ? size : 1);
    if (!p) qa_die("out of memory");
    return p;
}

void *qa_realloc(void *p, size_t size) {
    p = realloc(p, size ? size : 1);
    if (!p) qa_die("out of memory");
    return p;
}

char *qa_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s;
    if (vasprintf(&s, fmt, ap) < 0) qa_die("out of memory");
    va_end(ap);
    return s;
}

/* ---- bytes ---- */
void qa_put(qa_buf b[static 1], size_t n, const char s[n]) {
    if (b->n + n + 1 > b->cap) {
        b->cap = 2 * (b->n + n) + 64;
        b->p = qa_realloc(b->p, b->cap);
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void qa_putf(qa_buf b[static 1], const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s;
    int n = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (n < 0) qa_die("out of memory");
    qa_put(b, (size_t)n, s);
    free(s);
}

void qa_put_xml(qa_buf b[static 1], const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '<': qa_puts(b, "&lt;"); break;
        case '>': qa_puts(b, "&gt;"); break;
        case '&': qa_puts(b, "&amp;"); break;
        case '"': qa_puts(b, "&quot;"); break;
        case '\'': qa_puts(b, "&#x27;"); break;
        default: qa_put(b, 1, s);
        }
    }
}

uint64_t qa_hash(size_t n, const char s[n]) {
    uint64_t h = 0xcbf29ce484222325u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3u;
    return h;
}

void qa_push(qa_strs s[static 1], char *x) {
    if (s->n + 2 > s->cap) {
        s->cap = 2 * s->cap + 16;
        s->v = qa_realloc(s->v, s->cap * sizeof *s->v);
    }
    s->v[s->n++] = x;
    s->v[s->n] = nullptr;
}

/* ---- files ---- */
char *qa_try_read(const char *path, size_t len[static 1]) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    qa_buf b = {};
    qa_put(&b, 0, "");
    char chunk[1 << 16];
    for (size_t k; (k = fread(chunk, 1, sizeof chunk, f)) > 0;) qa_put(&b, k, chunk);
    if (ferror(f)) qa_die("%s: read failed", path);
    fclose(f);
    *len = b.n;
    return b.p;
}

char *qa_read(const char *path, size_t len[static 1]) {
    char *p = qa_try_read(path, len);
    if (!p) qa_die("%s: %m", path);
    return p;
}

void qa_write(const char *path, size_t n, const char data[n]) {
    FILE *f = fopen(path, "wb");
    if (!f) qa_die("%s: %m", path);
    bool ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) qa_die("%s: write failed", path);
}

void qa_mkdirs(const char *path) {
    char *p = qa_fmt("%s", path);
    for (char *s = p + 1;; s++) {
        if (*s && *s != '/') continue;
        char c = *s;
        *s = 0;
        if (mkdir(p, 0o777) != 0 && errno != EEXIST) qa_die("mkdir %s: %m", p);
        if (!c) break;
        *s = c;
    }
    free(p);
}

void qa_glob(qa_strs out[static 1], const char *pattern) {
    glob_t g;
    int rc = glob(pattern, 0, nullptr, &g);
    if (rc != 0 && rc != GLOB_NOMATCH) qa_die("glob %s failed", pattern);
    for (size_t i = 0; rc == 0 && i < g.gl_pathc; i++) qa_push(out, qa_fmt("%s", g.gl_pathv[i]));
    globfree(&g);
}

static const char *tmp_root(void) {
    const char *t = getenv("TMPDIR");
    return t && *t ? t : "/tmp";
}

char *qa_scratch(const char *name) {
    char *d = qa_fmt("%s/mmd-%u/%s", tmp_root(), (unsigned)getuid(), name);
    qa_mkdirs(d);
    return d;
}

char *qa_tmpdir(const char *name) {
    char *dir = qa_scratch(name), *d = qa_fmt("%s/XXXXXX", dir);
    free(dir);
    if (!mkdtemp(d)) qa_die("%s: %m", d);
    return d;
}

static int rm_one(const char *path, const struct stat *, int, struct FTW *) { return remove(path); }

void qa_rmtree(const char *path) {
    if (nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS) != 0) qa_die("removing %s: %m", path);
}

static bool is_root(const char *dir) {
    char p[PATH_MAX];
    return snprintf(p, sizeof p, "%s/include/mmd.h", dir) < (int)sizeof p && access(p, F_OK) == 0;
}

static char started_in[PATH_MAX];

char *qa_from_cwd(const char *path) {
    return *path == '/' || !*started_in ? qa_fmt("%s", path) : qa_fmt("%s/%s", started_in, path);
}

void qa_at_root(int argc, char *argv[]) {
    char cwd[PATH_MAX], exe[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) qa_die("getcwd: %m");
    strcpy(started_in, cwd);
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '/' && argv[i][0] != '-' && access(argv[i], F_OK) == 0) argv[i] = qa_fmt("%s/%s", cwd, argv[i]);
    /* the tree this was started in, else the one the program was built in */
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[k > 0 ? k : 0] = 0;
    for (char *d = cwd;; d = exe) {
        for (char *s; *d; *s = 0) {
            if (is_root(d)) {
                if (chdir(d) != 0) qa_die("chdir %s: %m", d);
                return;
            }
            if (!(s = strrchr(d, '/'))) break;
        }
        if (d == exe) qa_die("not inside the libmmd tree");
    }
}

/* ---- children ---- */
/* an unlinked scratch file: a child writing to one never blocks, whatever it writes */
static int scratch_fd(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/mmd-qa.XXXXXX", tmp_root());
    int fd = mkstemp(path);
    if (fd < 0) qa_die("%s: %m", path);
    unlink(path);
    return fd;
}

static char *take_fd(int fd, size_t len[static 1]) {
    struct stat st;
    if (fstat(fd, &st) != 0) qa_die("fstat: %m");
    size_t size = (size_t)st.st_size, n = 0;
    char *p = qa_alloc(size + 1);
    while (n < size) {
        ssize_t k = pread(fd, p + n, size - n, (off_t)n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) qa_die("pread: %m");
        n += (size_t)k;
    }
    close(fd);
    *len = n;
    return p;
}

qa_res qa_run(const qa_cmd c[static 1]) {
    int in, out = scratch_fd(), err = scratch_fd(), chk[2];
    if (c->in) {
        in = scratch_fd();
        for (size_t n = 0; n < c->in_len;) {
            ssize_t k = write(in, c->in + n, c->in_len - n);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) qa_die("write: %m");
            n += (size_t)k;
        }
        if (lseek(in, 0, SEEK_SET) != 0) qa_die("lseek: %m");
    } else if ((in = open("/dev/null", O_RDONLY)) < 0) qa_die("/dev/null: %m");
    if (pipe2(chk, O_CLOEXEC) != 0) qa_die("pipe: %m");

    pid_t pid = fork();
    if (pid < 0) qa_die("fork: %m");
    if (pid == 0) {
        /* async-signal-safe calls only: the parent may have threads (qa_parallel) */
        if (dup2(in, 0) >= 0 && dup2(out, 1) >= 0 && dup2(err, 2) >= 0) {
            alarm(c->timeout);
            execvp(c->argv[0], (char *const *)c->argv);
        }
        int e = errno;
        [[maybe_unused]] ssize_t w = write(chk[1], &e, sizeof e);
        _exit(127);
    }
    close(chk[1]);
    close(in);
    int e, st;
    ssize_t k;
    do k = read(chk[0], &e, sizeof e);
    while (k < 0 && errno == EINTR);
    close(chk[0]);
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) qa_die("waitpid: %m");
    if (k == sizeof e) qa_die("cannot run %s: %s", c->argv[0], strerror(e));

    qa_res r = {.code = WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st)};
    r.out = take_fd(out, &r.out_len);
    r.err = take_fd(err, &r.err_len);
    return r;
}

void qa_res_free(qa_res r[static 1]) {
    free(r->out);
    free(r->err);
    *r = (qa_res){};
}

typedef struct par {
    size_t n;
    _Atomic(size_t) next;
    void (*fn)(size_t i, void *ctx);
    void *ctx;
} par;

static void *par_run(void *arg) {
    par *p = arg;
    for (size_t i; (i = atomic_fetch_add(&p->next, 1)) < p->n;) p->fn(i, p->ctx);
    return nullptr;
}

void qa_parallel(size_t n, void (*fn)(size_t i, void *ctx), void *ctx) {
    pthread_t th[64];
    const char *env = getenv("QA_JOBS");
    long jobs = env ? atol(env) : sysconf(_SC_NPROCESSORS_ONLN);
    if (jobs > (long)countof(th) + 1) jobs = countof(th) + 1;
    if ((size_t)jobs > n) jobs = (long)n;
    par p = {.n = n, .fn = fn, .ctx = ctx};
    long started = 0;
    while (started < jobs - 1 && pthread_create(&th[started], nullptr, par_run, &p) == 0) started++;
    par_run(&p);
    for (long i = 0; i < started; i++) pthread_join(th[i], nullptr);
}

/* ---- Python's random.Random: MT19937 and the algorithms of CPython's Lib/random.py ---- */
enum { MT_N = 624, MT_M = 397 };

/* seed(int) is init_by_array() over the 32-bit digits of the seed: here, one */
void qa_seed(qa_rng r[static 1], uint32_t seed) {
    uint32_t *mt = r->mt;
    mt[0] = 19650218u;
    for (unsigned i = 1; i < MT_N; i++) mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
    unsigned i = 1;
    for (unsigned k = MT_N; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + seed;
        if (++i >= MT_N) { mt[0] = mt[MT_N - 1]; i = 1; }
    }
    for (unsigned k = MT_N - 1; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - i;
        if (++i >= MT_N) { mt[0] = mt[MT_N - 1]; i = 1; }
    }
    mt[0] = 0x80000000u;
    r->i = MT_N;
}

static uint32_t mt_next(qa_rng r[static 1]) {
    uint32_t *mt = r->mt;
    if (r->i >= MT_N) {
        for (unsigned k = 0; k < MT_N; k++) {
            uint32_t y = (mt[k] & 0x80000000u) | (mt[(k + 1) % MT_N] & 0x7fffffffu);
            mt[k] = mt[(k + MT_M) % MT_N] ^ (y >> 1) ^ (y & 1 ? 0x9908b0dfu : 0);
        }
        r->i = 0;
    }
    uint32_t y = mt[r->i++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    return y ^ (y >> 18);
}

double qa_random(qa_rng r[static 1]) {
    uint32_t a = mt_next(r) >> 5, b = mt_next(r) >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

/* _randbelow_with_getrandbits() */
uint32_t qa_below(qa_rng r[static 1], uint32_t n) {
    unsigned k = stdc_bit_width(n);
    uint32_t x;
    do x = mt_next(r) >> (32 - k);
    while (x >= n);
    return x;
}

void qa_sample(qa_rng r[static 1], uint32_t n, uint32_t k, uint32_t out[static k]) {
    uint32_t setsize = 21; /* below it CPython picks from a list, above it rejects repeats with a set */
    if (k > 5) {
        uint32_t p = 1;
        while (p < 3 * k) p *= 4;
        setsize += p;
    }
    if (n <= setsize) {
        uint32_t *pool = qa_alloc(n * sizeof *pool);
        for (uint32_t i = 0; i < n; i++) pool[i] = i;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t j = qa_below(r, n - i);
            out[i] = pool[j];
            pool[j] = pool[n - i - 1];
        }
        free(pool);
        return;
    }
    for (uint32_t i = 0; i < k; i++) {
        bool seen;
        do {
            out[i] = qa_below(r, n);
            seen = false;
            for (uint32_t j = 0; j < i; j++) seen |= out[j] == out[i];
        } while (seen);
    }
}

/* ---- mermaid.js and Chrome ---- */
bool qa_mermaid(const char *dir, const char *config, size_t n, const char *const files[static n], qa_render out[static n]) {
    qa_mkdirs(dir);
    qa_buf job = {};
    for (size_t i = 0; i < n; i++) {
        char *flat = qa_fmt("%s", files[i]);
        for (char *p = flat; *p; p++)
            if (*p == '/') *p = '_';
        out[i] = (qa_render){.svg = qa_fmt("%s/%s.svg", dir, flat), .err = qa_fmt("%s/%s.err", dir, flat)};
        free(flat);
        struct stat src, res;
        if (stat(files[i], &src) != 0) qa_die("%s: %m", files[i]);
        bool fresh = (stat(out[i].svg, &res) == 0 || stat(out[i].err, &res) == 0) && res.st_mtime >= src.st_mtime;
        if (!fresh) qa_putf(&job, "%s\t%s\t%s\n", files[i], out[i].svg, out[i].err);
    }
    char *css = qa_fmt("%s/mmdc.css", dir);
    if (job.n || access(css, F_OK) != 0) {
        qa_res r = qa_run(&(qa_cmd){.argv = QA_ARGV("node", "tools/mermaid_render.mjs", config ? config : "{}", css),
                                    .in = job.n ? job.p : "", .in_len = job.n});
        if (r.code == 3) return false;
        if (r.code != 0) qa_die("tools/mermaid_render.mjs failed:\n%s", r.err);
        qa_res_free(&r);
    }
    free(css);
    free(job.p);
    for (size_t i = 0; i < n; i++) {
        size_t len;
        char **gone = access(out[i].svg, F_OK) == 0 ? &out[i].err : &out[i].svg;
        if (gone == &out[i].svg) {
            char *why = qa_read(out[i].err, &len);
            free(out[i].err);
            out[i].err = why;
        }
        free(*gone);
        *gone = nullptr;
    }
    return true;
}

char *qa_which(const char *name) {
    const char *path = getenv("PATH");
    for (const char *d = path ? path : ""; *d;) {
        size_t n = strcspn(d, ":");
        char *p = qa_fmt("%.*s/%s", (int)n, d, name);
        if (access(p, X_OK) == 0) return p;
        free(p);
        d += n + (d[n] == ':');
    }
    return nullptr;
}

bool qa_svg_size(const char *svg, double w[static 1], double h[static 1]) {
    const char *tag = strstr(svg, "<svg"), *end = tag ? strchr(tag, '>') : nullptr;
    const char *v = end ? memmem(tag, (size_t)(end - tag), " viewBox=\"", 10) : nullptr;
    if (!v) return false;
    char *p;
    strtod(v + 10, &p);
    strtod(p, &p);
    *w = strtod(p, &p);
    *h = strtod(p, &p);
    return *p == '"' && *w > 0 && *h > 0;
}

const char *qa_chrome(void) {
    static const char *const names[] = {"google-chrome", "chromium", "chromium-browser"};
    const char *exe = getenv("CHROME");
    if (exe && *exe) return exe;
    for (size_t i = 0; i < countof(names); i++)
        if ((exe = qa_which(names[i]))) return exe;
    qa_die("no google-chrome, chromium or chromium-browser on PATH: set CHROME");
}

/* Chrome leaves profile directories in TMPDIR: give it one of its own and remove that */
static qa_res chrome(const char *page, const char *a1, const char *a2) {
    char *tmp = qa_tmpdir("chrome"), full[PATH_MAX];
    if (!realpath(page, full)) qa_die("%s: %m", page);
    char *env = qa_fmt("TMPDIR=%s", tmp), *url = qa_fmt("file://%s", full);
    qa_res r = qa_run(&(qa_cmd){.argv = QA_ARGV("env", env, qa_chrome(), "--headless", "--no-sandbox", "--disable-gpu",
                                                "--hide-scrollbars", a1, a2, url),
                                .timeout = 300});
    if (r.code != 0) qa_die("%s failed on %s:\n%s", qa_chrome(), page, r.err);
    qa_rmtree(tmp);
    free(tmp);
    free(env);
    free(url);
    return r;
}

void qa_screenshot(const char *page, unsigned w, unsigned h, const char *png) {
    char *size = qa_fmt("--window-size=%u,%u", w, h), *shot = qa_fmt("--screenshot=%s", png);
    qa_res r = chrome(page, size, shot);
    qa_res_free(&r);
    free(size);
    free(shot);
}

char *qa_dump_dom(const char *page) {
    qa_res r = chrome(page, "--virtual-time-budget=5000", "--dump-dom");
    free(r.err);
    return r.out;
}
