/* cmp-audio: agreement between a reference and a test signal, raw mono.
 *   cmp-audio [opts] REF TEST
 *     -r s16        REF is s16le (e.g. deep-filter's wav output), else float32
 *     -q            compare in int16: TEST is converted the way deep-filter's
 *                   wav writer does it, (sample * 32767) as i16 (truncate
 *                   toward zero, saturate); REF must be -r s16
 *     -o N          fixed offset: TEST[n + N] is compared with REF[n]
 *     -L N          else search the offset in [-N, N] (default 2000), by least
 *                   squared error over the loudest 4 s of REF
 *     -s SEC        segment length for the per-segment table (default 5)
 *     -S N          per segment, search the offset again within +-N of the
 *                   global one (for a plugin whose latency moves)
 *     -k N          skip the first N samples of REF in all statistics
 *     -w SEC -S N   tracking: windows of SEC seconds, each aligned within +-N
 *                   of the previous window's offset (silent windows keep it);
 *                   prints every offset change and the SNR over all windows
 *                   at their own offsets
 * SNR = 10 log10(sum ref^2 / sum (ref - test)^2) in dB.
 * Build: gcc -O2 -o cmp-audio cmp-audio.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float *load(const char *path, int s16, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long b = ftell(f);
    fseek(f, 0, SEEK_SET);
    long k = b / (s16 ? 2 : 4);
    float *x = malloc(k * sizeof(float));
    if (s16) {
        short *t = malloc(k * 2);
        if (fread(t, 2, k, f) != (size_t)k) { fprintf(stderr, "%s: short read\n", path); exit(1); }
        for (long i = 0; i < k; i++) x[i] = t[i];
        free(t);
    } else if (fread(x, 4, k, f) != (size_t)k) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
    *n = k;
    return x;
}

static int q16(float s) {
    float v = s * 32767.f;
    if (v != v) return 0;
    if (v >= 32767.f) return 32767;
    if (v <= -32768.f) return -32768;
    return (int)v;
}

static long nr, nt;
static float *R, *T;
static int quant;

static double tv(long j) { return quant ? (double)q16(T[j]) : (double)T[j]; }

/* squared error of TEST shifted by lag against REF over [a, b) */
static double err_at(long lag, long a, long b, double *eref) {
    double e = 0, r2 = 0;
    for (long i = a; i < b; i++) {
        long j = i + lag;
        double t = (j >= 0 && j < nt) ? tv(j) : 0.0;
        double dd = R[i] - t;
        e += dd * dd;
        r2 += (double)R[i] * R[i];
    }
    if (eref) *eref = r2;
    return e;
}

static long best_lag(long lo, long hi, long a, long b) {
    long best = lo;
    double be = -1;
    for (long l = lo; l <= hi; l++) {
        double e = err_at(l, a, b, NULL);
        if (be < 0 || e < be) { be = e; best = l; }
    }
    return best;
}

static double db(double num, double den) {
    if (den <= 0) return INFINITY;
    if (num <= 0) return -INFINITY;
    return 10 * log10(num / den);
}

int main(int argc, char **argv) {
    int s16 = 0, have_off = 0;
    long off = 0, maxlag = 2000, seglag = 0, skip = 0;
    double segs = 5, wsec = 0;
    int a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        const char *o = argv[a];
        if (!strcmp(o, "-r") && a + 1 < argc) s16 = !strcmp(argv[++a], "s16");
        else if (!strcmp(o, "-q")) quant = 1;
        else if (!strcmp(o, "-o") && a + 1 < argc) { off = atol(argv[++a]); have_off = 1; }
        else if (!strcmp(o, "-L") && a + 1 < argc) maxlag = atol(argv[++a]);
        else if (!strcmp(o, "-s") && a + 1 < argc) segs = atof(argv[++a]);
        else if (!strcmp(o, "-S") && a + 1 < argc) seglag = atol(argv[++a]);
        else if (!strcmp(o, "-k") && a + 1 < argc) skip = atol(argv[++a]);
        else if (!strcmp(o, "-w") && a + 1 < argc) wsec = atof(argv[++a]);
        else { fprintf(stderr, "cmp-audio: unknown option %s\n", o); return 2; }
    }
    if (a + 2 != argc) { fprintf(stderr, "usage: cmp-audio [opts] REF TEST\n"); return 2; }
    if (quant && !s16) { fprintf(stderr, "cmp-audio: -q needs -r s16\n"); return 2; }
    R = load(argv[a], s16, &nr);
    T = load(argv[a + 1], 0, &nt);
    double scale = s16 && !quant ? 1.0 / 32767.0 : 1.0;  /* s16 ref vs float test */
    if (scale != 1.0)
        for (long i = 0; i < nr; i++) R[i] *= scale;

    if (!have_off) {                 /* loudest 4 s window of REF */
        long w = 4 * 48000, bi = skip;
        double be = -1, e = 0;
        if (w > nr - skip) w = nr - skip;
        for (long i = skip; i < skip + w; i++) e += (double)R[i] * R[i];
        be = e;
        for (long i = skip + w; i < nr; i++) {
            e += (double)R[i] * R[i] - (double)R[i - w] * R[i - w];
            if (e > be && (i - w + 1) % 4800 == 0) { be = e; bi = i - w + 1; }
        }
        off = best_lag(-maxlag, maxlag, bi, bi + w);
        printf("offset: %ld samples (TEST lags REF by %ld; searched +-%ld over REF [%.2f s, %.2f s))\n",
               off, off, maxlag, bi / 48000.0, (bi + w) / 48000.0);
    } else printf("offset: %ld samples (fixed)\n", off);

    if (wsec > 0 && seglag > 0) {       /* tracking mode */
        long w = (long)(wsec * 48000), lag = off, nchg = 0, nw = 0;
        double r2 = 0, e2 = 0, maxd = 0;
        double *wsnr = malloc(sizeof(double) * (nr / w + 2));
        double *wr = calloc(nr / w + 2, sizeof(double)), *we = calloc(nr / w + 2, sizeof(double));
        long *wl = calloc(nr / w + 2, sizeof(long));
        long nws = 0;
        printf("tracking: %.3f s windows, +-%ld around the previous offset\n", wsec, seglag);
        for (long s = skip; s + w <= nr; s += w) {
            double er, ee;
            err_at(lag, s, s + w, &er);
            if (er / w > (quant ? 32767.0 * 32767.0 : 1.0) * 1e-7) {   /* not silent */
                long l = best_lag(lag - seglag, lag + seglag, s, s + w);
                if (l != lag) {
                    printf("  %.3f s: offset %ld -> %ld\n", s / 48000.0, lag, l);
                    lag = l;
                    nchg++;
                }
            }
            ee = err_at(lag, s, s + w, &er);
            r2 += er;
            e2 += ee;
            nw++;
            if (er > 0) wsnr[nws++] = db(er, ee);
            wr[nw - 1] = er; we[nw - 1] = ee; wl[nw - 1] = lag;
            for (long i = s; i < s + w; i++) {
                long j = i + lag;
                double dd = fabs(R[i] - ((j >= 0 && j < nt) ? tv(j) : 0.0));
                if (dd > maxd) maxd = dd;
            }
        }
        for (long i = 1; i < nws; i++)      /* insertion sort, few hundred */
            for (long j = i; j > 0 && wsnr[j - 1] > wsnr[j]; j--) {
                double t = wsnr[j]; wsnr[j] = wsnr[j - 1]; wsnr[j - 1] = t;
            }
        printf("tracked: %ld windows, %ld offset changes, final offset %ld\n", nw, nchg, lag);
        double sr2 = 0, se2 = 0;           /* without windows next to a change */
        long kept = 0;
        for (long i = 0; i < nw; i++) {
            if ((i > 0 && wl[i - 1] != wl[i]) || (i + 1 < nw && wl[i + 1] != wl[i])) continue;
            sr2 += wr[i]; se2 += we[i]; kept++;
        }
        printf("SNR over the %ld windows not adjacent to an offset change: %.2f dB\n", kept, db(sr2, se2));
        printf("SNR over all windows at their offsets: %.2f dB; per-window SNR p10 %.2f, median %.2f, p90 %.2f dB; max |diff| %.3g%s\n",
               db(r2, e2), wsnr[nws / 10], wsnr[nws / 2], wsnr[nws * 9 / 10], maxd, quant ? " LSB" : "");
        return 0;
    }

    long a0 = skip, b0 = nr;
    if (b0 + off > nt) b0 = nt - off;
    if (a0 + off < 0) a0 = -off;
    double r2 = 0, e2 = 0, maxd = 0, t2 = 0, rt = 0;
    long maxi = -1, same = 0, h1 = 0, h2 = 0, hm = 0;
    for (long i = a0; i < b0; i++) {
        double t = tv(i + off), dd = fabs(R[i] - t);
        r2 += (double)R[i] * R[i];
        t2 += t * t;
        rt += R[i] * t;
        e2 += dd * dd;
        if (dd > maxd) { maxd = dd; maxi = i; }
        if (dd == 0) same++;
        else if (dd <= 1) h1++;
        else if (dd <= 2) h2++;
        else hm++;
    }
    long cnt = b0 - a0;
    printf("compared: %ld samples (%.2f s), REF rms %.2f dBFS\n", cnt, cnt / 48000.0,
           db(r2 / cnt, quant ? 32767.0 * 32767.0 : 1.0));
    printf("SNR of difference vs REF: %.2f dB; correlation %.6f\n", db(r2, e2), rt / sqrt(r2 * t2));
    if (quant) {
        printf("int16: identical %.4f%%, |diff| 1 LSB %.4f%%, 2 LSB %.4f%%, >2 LSB %.4f%%; max |diff| %.0f LSB at %.4f s\n",
               100.0 * same / cnt, 100.0 * h1 / cnt, 100.0 * h2 / cnt, 100.0 * hm / cnt, maxd, maxi / 48000.0);
    } else {
        printf("max |diff| %.3g (%.2f dBFS) at %.4f s; identical samples %.4f%%\n", maxd,
               maxd > 0 ? 20 * log10(maxd) : -INFINITY, maxi / 48000.0, 100.0 * same / cnt);
    }

    long sl = (long)(segs * 48000);
    printf("%8s %8s %9s %10s %12s%s\n", "start_s", "ref_dBFS", "SNR_dB", "max_diff", quant ? "ident_%" : "max_dBFS",
           seglag ? "  local_offset SNR_local_dB" : "");
    for (long s = a0; s < b0; s += sl) {
        long e = s + sl < b0 ? s + sl : b0;
        double sr2 = 0, se2 = 0, smd = 0;
        long sid = 0;
        for (long i = s; i < e; i++) {
            double t = tv(i + off), dd = fabs(R[i] - t);
            sr2 += (double)R[i] * R[i];
            se2 += dd * dd;
            if (dd > smd) smd = dd;
            if (dd == 0) sid++;
        }
        printf("%8.2f %8.2f %9.2f %10.3g %12.4f", s / 48000.0,
               db(sr2 / (e - s), quant ? 32767.0 * 32767.0 : 1.0), db(sr2, se2), smd,
               quant ? 100.0 * sid / (e - s) : (smd > 0 ? 20 * log10(smd) : -INFINITY));
        if (seglag) {
            long l = best_lag(off - seglag, off + seglag, s, e);
            double rr, ee = err_at(l, s, e, &rr);
            printf("  %12ld %12.2f", l, db(rr, ee));
        }
        printf("\n");
    }
    return 0;
}
