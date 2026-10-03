/* lens-lines: per line of the stream (tokens at consecutive positions up to
 * one holding a newline), the J-lens words (mind.log l27/l29/l31) summed as
 * probability over the line's tokens and blocks, divided by tokens x blocks,
 * leaving out words the line itself holds. Prints the top unsaid word's
 * weight per line, then quantiles and shares over thresholds.
 * Build: tcc -o lens-lines lens-lines.c   Run: ./lens-lines mind.log [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define MAXW 512
static char word[MAXW][48];
static double wt[MAXW];
static int nw;
static char said[8192];
static int ntok, nlay;
static double tops[400000];
static int ntops;
static int verbose;

static void lower(char *s) { for (; *s; s++) *s = tolower((unsigned char)*s); }

static void add(const char *w, double p) {
    for (int i = 0; i < nw; i++)
        if (!strcmp(word[i], w)) { wt[i] += p; return; }
    if (nw < MAXW) { strncpy(word[nw], w, 47); word[nw][47] = 0; wt[nw++] = p; }
}

static void flush(void) {
    if (ntok >= 3 && nlay > 0) {
        double best = 0; int bi = -1;
        for (int i = 0; i < nw; i++) {
            if (strlen(word[i]) < 3) continue;
            char pad[64]; snprintf(pad, sizeof pad, "%s", word[i]);
            if (strstr(said, pad)) continue;
            double v = wt[i] / nlay;
            if (v > best) { best = v; bi = i; }
        }
        if (ntops < 400000) tops[ntops++] = best;
        if (verbose && bi >= 0) printf("%.3f %s | %.80s\n", best, word[bi], said);
    }
    nw = 0; said[0] = 0; ntok = 0; nlay = 0;
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "lens-lines mind.log [-v]\n"); return 1; }
    verbose = argc > 2 && !strcmp(argv[2], "-v");
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    static char line[1 << 16];
    long last = -10;
    while (fgets(line, sizeof line, f)) {
        char *pp = strstr(line, " pos=");
        char *tp = strstr(line, " tok=");
        if (!pp || !tp) continue;
        long pos = atol(pp + 5);
        if (pos != last + 1) flush();
        last = pos;
        /* the token's text, unescaped enough to match words */
        char tok[256]; int k = 0; int nl = 0;
        for (char *p = tp + 5; *p && *p != ' ' && *p != '\n' && k < 250; p++) {
            if (p[0] == '\\' && p[1] == 's') { tok[k++] = ' '; p++; }
            else if (p[0] == '\\' && p[1] == 'n') { nl = 1; tok[k++] = ' '; p++; }
            else tok[k++] = *p;
        }
        tok[k] = 0; lower(tok);
        if (strlen(said) + strlen(tok) < sizeof said - 1) strcat(said, tok);
        ntok++;
        /* blocks: lNN=w:lp,w:lp */
        for (char *q = strstr(line, " l"); q; q = strstr(q + 1, " l")) {
            if (!isdigit((unsigned char)q[2])) continue;
            char *eq = strchr(q, '='); if (!eq) break;
            char *end = strchr(eq, ' '); if (!end) end = eq + strlen(eq);
            nlay++;
            char buf[4096]; int n = end - eq - 1; if (n > 4095) n = 4095;
            memcpy(buf, eq + 1, n); buf[n] = 0;
            for (char *s = strtok(buf, ","); s; s = strtok(NULL, ",")) {
                char *c = strrchr(s, ':'); if (!c) continue;
                *c = 0; double lp = atof(c + 1);
                char w[48]; snprintf(w, sizeof w, "%s", s); lower(w);
                add(w, exp(lp));
            }
        }
        if (nl) flush();
    }
    flush();
    qsort(tops, ntops, sizeof *tops, cmp);
    printf("lines %d\n", ntops);
    double qs[] = {0.25, 0.5, 0.75, 0.9, 0.95};
    for (int i = 0; i < 5; i++) printf("q%.2f %.3f\n", qs[i], ntops ? tops[(int)(qs[i] * (ntops - 1))] : 0);
    double th[] = {0.05, 0.1, 0.15, 0.2, 0.3, 0.5};
    for (int i = 0; i < 6; i++) {
        int c = 0; for (int j = 0; j < ntops; j++) c += tops[j] >= th[i];
        printf(">= %.2f: %.1f%%\n", th[i], ntops ? 100.0 * c / ntops : 0);
    }
    return 0;
}
