/* ladspa-run: a tiny offline LADSPA host. dlopens a plugin, connects every
 * port (control inputs start at their default hint, as PipeWire's
 * filter-chain does, then -c overrides), runs a raw float32 mono file
 * through it in fixed or random block sizes and writes the output raw.
 *   ladspa-run -l PLUGIN.so                       list descriptors and ports
 *   ladspa-run [opts] PLUGIN.so LABEL IN.f32 OUT.f32
 *     -r RATE         sample rate passed to instantiate (48000)
 *     -b N            block size (256)
 *     -b rand:SEED[:MAX]  random block sizes 1..MAX (2048), LCG seeded
 *     -c 'Port name=VALUE'   set a control input (repeatable)
 *     -p N            N passes over the input; between passes deactivate +
 *                     activate the same instance; OUT holds every pass and
 *                     each pass is compared byte for byte with the first
 *     -P N            like -p but a new instance (cleanup + instantiate)
 *     -t FILE         per-run() thread CPU times in microseconds, one per line
 *     -i              in-place: audio in and out share one buffer
 *     -R              real-time pacing: each run() starts no earlier than its
 *                     block's place on the audio clock, as a PipeWire graph
 *                     calls it (needed by plugins that process on their own
 *                     thread, like the Rust deep-filter-ladspa)
 * Timing: thread CPU time (CLOCK_THREAD_CPUTIME_ID) around each run() call,
 * plus process CPU (getrusage) which also counts a plugin's own threads.
 * The plugin is never dlclosed (the Rust plugin's worker thread lives on).
 * Build: gcc -O2 -o ladspa-run ladspa-run.c -ldl -lm */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include "ladspa-abi.h"

static double now_cpu(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static double now_wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static double proc_cpu(void) {
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec * 1e-6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec * 1e-6;
}

static float hint_default(const LADSPA_PortRangeHint *h, unsigned long rate) {
    int d = h->HintDescriptor;
    float lo = h->LowerBound, hi = h->UpperBound;
    if (d & LADSPA_HINT_SAMPLE_RATE) { lo *= rate; hi *= rate; }
    int lg = (d & LADSPA_HINT_LOGARITHMIC) && lo > 0 && hi > 0;
    float v;
    switch (d & LADSPA_HINT_DEFAULT_MASK) {
    case LADSPA_HINT_DEFAULT_MINIMUM: v = lo; break;
    case LADSPA_HINT_DEFAULT_LOW: v = lg ? expf(logf(lo) * .75f + logf(hi) * .25f) : lo * .75f + hi * .25f; break;
    case LADSPA_HINT_DEFAULT_MIDDLE: v = lg ? expf(logf(lo) * .5f + logf(hi) * .5f) : lo * .5f + hi * .5f; break;
    case LADSPA_HINT_DEFAULT_HIGH: v = lg ? expf(logf(lo) * .25f + logf(hi) * .75f) : lo * .25f + hi * .75f; break;
    case LADSPA_HINT_DEFAULT_MAXIMUM: v = hi; break;
    case LADSPA_HINT_DEFAULT_0: v = 0; break;
    case LADSPA_HINT_DEFAULT_1: v = 1; break;
    case LADSPA_HINT_DEFAULT_100: v = 100; break;
    case LADSPA_HINT_DEFAULT_440: v = 440; break;
    default: v = (d & LADSPA_HINT_BOUNDED_BELOW) ? lo : 0; break;
    }
    if (d & LADSPA_HINT_INTEGER) v = roundf(v);
    return v;
}

static const char *hint_name(int d) {
    switch (d & LADSPA_HINT_DEFAULT_MASK) {
    case LADSPA_HINT_DEFAULT_NONE: return "none";
    case LADSPA_HINT_DEFAULT_MINIMUM: return "minimum";
    case LADSPA_HINT_DEFAULT_LOW: return "low";
    case LADSPA_HINT_DEFAULT_MIDDLE: return "middle";
    case LADSPA_HINT_DEFAULT_HIGH: return "high";
    case LADSPA_HINT_DEFAULT_MAXIMUM: return "maximum";
    case LADSPA_HINT_DEFAULT_0: return "0";
    case LADSPA_HINT_DEFAULT_1: return "1";
    case LADSPA_HINT_DEFAULT_100: return "100";
    case LADSPA_HINT_DEFAULT_440: return "440";
    }
    return "?";
}

static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static unsigned long long lcg;
static unsigned next_rand(void) {
    lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)(lcg >> 33);
}

int main(int argc, char **argv) {
    unsigned long rate = 48000;
    int block = 256, rnd = 0, rmax = 2048, passes = 1, fresh = 0, list = 0, pace = 0, inplace = 0;
    const char *ctl[64], *tfile = NULL;
    int nctl = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        const char *o = argv[a];
        if (!strcmp(o, "-l")) list = 1;
        else if (!strcmp(o, "-r") && a + 1 < argc) rate = strtoul(argv[++a], NULL, 10);
        else if (!strcmp(o, "-b") && a + 1 < argc) {
            const char *v = argv[++a];
            if (!strncmp(v, "rand:", 5)) {
                char *e;
                rnd = 1;
                lcg = strtoull(v + 5, &e, 10);
                if (*e == ':') rmax = atoi(e + 1);
            } else block = atoi(v);
        } else if (!strcmp(o, "-c") && a + 1 < argc && nctl < 64) ctl[nctl++] = argv[++a];
        else if (!strcmp(o, "-p") && a + 1 < argc) passes = atoi(argv[++a]);
        else if (!strcmp(o, "-P") && a + 1 < argc) { passes = atoi(argv[++a]); fresh = 1; }
        else if (!strcmp(o, "-t") && a + 1 < argc) tfile = argv[++a];
        else if (!strcmp(o, "-R")) pace = 1;
        else if (!strcmp(o, "-i")) inplace = 1;
        else { fprintf(stderr, "ladspa-run: unknown option %s\n", o); return 2; }
    }
    if (a >= argc) { fprintf(stderr, "usage: ladspa-run -l PLUGIN | [opts] PLUGIN LABEL IN.f32 OUT.f32\n"); return 2; }
    void *so = dlopen(argv[a], RTLD_NOW | RTLD_LOCAL);
    if (!so) { fprintf(stderr, "ladspa-run: %s\n", dlerror()); return 1; }
    LADSPA_Descriptor_Function df = (LADSPA_Descriptor_Function)dlsym(so, "ladspa_descriptor");
    if (!df) { fprintf(stderr, "ladspa-run: no ladspa_descriptor in %s\n", argv[a]); return 1; }
    if (list) {
        const LADSPA_Descriptor *d;
        for (unsigned long i = 0; (d = df(i)); i++) {
            printf("[%lu] id=%lu label=%s name=\"%s\" maker=\"%s\" props=0x%x ports=%lu\n", i,
                   d->UniqueID, d->Label, d->Name, d->Maker, d->Properties, d->PortCount);
            for (unsigned long p = 0; p < d->PortCount; p++) {
                int pd = d->PortDescriptors[p];
                const LADSPA_PortRangeHint *h = &d->PortRangeHints[p];
                printf("  %lu %s %s \"%s\" hint=0x%x bounds=[%g, %g] default=%s (%g)\n", p,
                       pd & LADSPA_PORT_AUDIO ? "audio" : "control",
                       pd & LADSPA_PORT_INPUT ? "in" : "out", d->PortNames[p], h->HintDescriptor,
                       h->LowerBound, h->UpperBound, hint_name(h->HintDescriptor), hint_default(h, rate));
            }
        }
        return 0;
    }
    if (a + 3 >= argc) { fprintf(stderr, "ladspa-run: need PLUGIN LABEL IN OUT\n"); return 2; }
    const char *label = argv[a + 1], *inp = argv[a + 2], *outp = argv[a + 3];
    const LADSPA_Descriptor *d = NULL;
    for (unsigned long i = 0; (d = df(i)); i++)
        if (!strcmp(d->Label, label)) break;
    if (!d) { fprintf(stderr, "ladspa-run: no label %s\n", label); return 1; }

    FILE *f = fopen(inp, "rb");
    if (!f) { perror(inp); return 1; }
    fseek(f, 0, SEEK_END);
    long nbytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    long n = nbytes / 4;
    float *x = malloc(n * 4), *y = calloc((size_t)n * passes, 4);
    if (fread(x, 4, n, f) != (size_t)n) { fprintf(stderr, "ladspa-run: short read\n"); return 1; }
    fclose(f);

    int ain = -1, aout = -1;
    float *cv = calloc(d->PortCount, sizeof(float));
    for (unsigned long p = 0; p < d->PortCount; p++) {
        int pd = d->PortDescriptors[p];
        if (pd & LADSPA_PORT_AUDIO) {
            if ((pd & LADSPA_PORT_INPUT) && ain < 0) ain = p;
            if ((pd & LADSPA_PORT_OUTPUT) && aout < 0) aout = p;
        } else cv[p] = hint_default(&d->PortRangeHints[p], rate);
    }
    for (int i = 0; i < nctl; i++) {
        const char *eq = strrchr(ctl[i], '=');
        int hit = 0;
        for (unsigned long p = 0; eq && p < d->PortCount; p++)
            if (strlen(d->PortNames[p]) == (size_t)(eq - ctl[i]) && !strncmp(d->PortNames[p], ctl[i], eq - ctl[i])) {
                cv[p] = strtof(eq + 1, NULL);
                hit = 1;
            }
        if (!hit) { fprintf(stderr, "ladspa-run: no control '%s'\n", ctl[i]); return 1; }
    }
    if (ain < 0 || aout < 0) { fprintf(stderr, "ladspa-run: plugin lacks mono audio in/out\n"); return 1; }
    fprintf(stderr, "ladspa-run: %s %s, %ld samples, block %s%d, passes %d (%s)\n", argv[a], label, n,
            rnd ? "rand 1.." : "", rnd ? rmax : block, passes, fresh ? "new instance" : "deactivate+activate");
    for (unsigned long p = 0; p < d->PortCount; p++)
        if (!(d->PortDescriptors[p] & LADSPA_PORT_AUDIO))
            fprintf(stderr, "  control \"%s\" = %g\n", d->PortNames[p], cv[p]);

    int maxb = rnd ? rmax : block;
    float *bin = malloc(maxb * 4), *bout = malloc(maxb * 4);
    size_t ncall = 0, capcall = (size_t)(n / 1 + 16) * passes;
    double *tcall = malloc(capcall * sizeof(double));
    int *bcall = malloc(capcall * sizeof(int));
    double pc0 = proc_cpu(), w0 = now_wall();
    LADSPA_Handle h = NULL;
    int rc = 0;
    for (int pass = 0; pass < passes; pass++) {
        if (!h || fresh) {
            if (h) { if (d->deactivate) d->deactivate(h); d->cleanup(h); }
            h = d->instantiate(d, rate);
            if (!h) { fprintf(stderr, "ladspa-run: instantiate failed\n"); return 1; }
            for (unsigned long p = 0; p < d->PortCount; p++)
                if (!(d->PortDescriptors[p] & LADSPA_PORT_AUDIO)) d->connect_port(h, p, &cv[p]);
            d->connect_port(h, ain, bin);
            d->connect_port(h, aout, inplace ? bin : bout);
            if (d->activate) d->activate(h);
        } else {
            if (d->deactivate) d->deactivate(h);
            if (d->activate) d->activate(h);
        }
        float *yp = y + (size_t)pass * n;
        for (long i = 0; i < n;) {
            int b = rnd ? 1 + (int)(next_rand() % rmax) : block;
            if (b > n - i) b = n - i;
            memcpy(bin, x + i, b * 4);
            if (pace) {
                double due = w0 + ((double)pass * n + i) / rate;
                struct timespec ts = {(time_t)due, (long)((due - (time_t)due) * 1e9)};
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
            }
            double t0 = now_cpu();
            d->run(h, b);
            double t1 = now_cpu();
            if (ncall < capcall) { tcall[ncall] = t1 - t0; bcall[ncall] = b; ncall++; }
            memcpy(yp + i, inplace ? bin : bout, b * 4);
            i += b;
        }
        if (pass > 0) {
            int same = !memcmp(yp, y, (size_t)n * 4);
            long first = -1;
            for (long i = 0; !same && i < n && first < 0; i++)
                if (memcmp(&yp[i], &y[i], 4)) first = i;
            fprintf(stderr, "ladspa-run: pass %d vs pass 1: %s", pass + 1, same ? "byte-identical\n" : "DIFFERENT");
            if (!same) { fprintf(stderr, " (first at sample %ld)\n", first); rc = 3; }
        }
    }
    double pc1 = proc_cpu(), w1 = now_wall();
    if (h) { if (d->deactivate) d->deactivate(h); }
    FILE *o = fopen(outp, "wb");
    if (!o || fwrite(y, 4, (size_t)n * passes, o) != (size_t)n * passes) { perror(outp); return 1; }
    fclose(o);

    double tot = 0;
    for (size_t i = 0; i < ncall; i++) tot += tcall[i];
    double hops = (double)n * passes / 480.0;
    double *s = malloc(ncall * sizeof(double));
    memcpy(s, tcall, ncall * sizeof(double));
    qsort(s, ncall, sizeof(double), cmpd);
    fprintf(stderr, "ladspa-run: run() thread CPU: total %.3f s, %.1f us per 480-sample hop; per call mean %.1f us, p50 %.1f, p99 %.1f, max %.1f us (%zu calls)\n",
            tot, tot / hops * 1e6, tot / ncall * 1e6, s[ncall / 2] * 1e6, s[(size_t)(ncall * 0.99)] * 1e6,
            s[ncall - 1] * 1e6, ncall);
    fprintf(stderr, "ladspa-run: process CPU %.3f s (%.1f us per hop, all threads), wall %.3f s, audio %.3f s\n",
            pc1 - pc0, (pc1 - pc0) / hops * 1e6, w1 - w0, hops * 0.01);
    if (tfile) {
        FILE *t = fopen(tfile, "w");
        for (size_t i = 0; t && i < ncall; i++) fprintf(t, "%d\t%.3f\n", bcall[i], tcall[i] * 1e6);
        if (t) fclose(t);
    }
    fflush(stderr);
    _exit(rc);   /* no dlclose, no destructors racing a plugin's threads */
}
