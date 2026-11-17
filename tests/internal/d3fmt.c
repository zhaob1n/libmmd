/* d3fmt against the real d3-format: tests/d3fmt_cases.tsv (tools/d3fmt_cases.mjs) holds
 * spec \t value bits \t d3.format(spec)(value), "!" where d3 rejects the spec. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3fmt.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/d3fmt_cases.tsv";
    FILE *in = fopen(path, "r");
    if (!in) return perror(path), 1;
    char line[4096];
    unsigned n = 0, bad = 0;
    Buf b = {};
    while (fgets(line, sizeof line, in)) {
        line[strcspn(line, "\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
        if (!t2) continue;
        *t1 = *t2 = 0;
        uint64_t bits = strtoull(t1 + 1, nullptr, 16);
        double v;
        memcpy(&v, &bits, sizeof v);
        D3Fmt f;
        bool ok = d3fmt_parse((Str){.p = line, .n = strlen(line)}, &f);
        b.n = 0;
        if (ok) d3fmt(&b, &f, v);
        if (!buf_reserve(&b, 1)) return 1;
        b.p[b.n] = 0;
        const char *got = ok ? b.p : "!";
        n++;
        if (strcmp(got, t2 + 1) && bad++ < 40) printf("FAIL d3fmt \"%s\"(%.17g): \"%s\", d3 \"%s\"\n", line, v, got, t2 + 1);
    }
    fclose(in);
    free(b.p);
    printf("d3fmt: %u cases, %u failed\n", n, bad);
    return bad != 0;
}
