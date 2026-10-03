/* dfn3.c: DeepFilterNet3 noise suppression, streaming, from the MIT weights
 * published as model.safetensors (mlx-community/DeepFilterNet-mlx v3, same
 * bytes as iky1e/DeepFilterNet3-MLX). Follows Rikorose/DeepFilterNet v0.5.6:
 *   network  DeepFilterNet/df/deepfilternet3.py + df/modules.py
 *   DSP      libDF/src/lib.rs (Vorbis-window STFT, ERB features with
 *            exponential mean norm, complex features with unit norm)
 *   loop     libDF/src/tract.rs DfTract::process (rolling spectra, ERB
 *            mask on frame t-2, deep filter over frames t-4..t, thresholds,
 *            post filter, attenuation limit, silence gate)
 * The safetensors hold the PyTorch state_dict unchanged (the converter does
 * no transpose): Conv2d [out, in/g, kt, kf], ConvTranspose2d [in, out/g,
 * kt, kf], GroupedLinear [g, in/g, out/g], GRU [3H, in] with gates r,z,n,
 * BatchNorm weight/bias/running_mean/running_var (eps 1e-5, PyTorch's).
 * Safetensors parsed by hand: 8-byte little-endian header length, a JSON
 * header, then raw little-endian tensors.
 * Causal in time like tract's pulsed models: conv histories and GRU states
 * carry over between hops; a decoder's state only advances on hops where it
 * runs, as tract's do. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dfn3.h"

/* DeepFilterNet3 defaults; dfn3_new checks config.json against them. */
#define SR 48000
#define FFT 960
#define HOP 480
#define NF 481                  /* fft/2 + 1 */
#define NE 32                   /* nb_erb */
#define ND 96                   /* nb_df */
#define ORD 5                   /* df_order */
#define CLA 2                   /* conv_lookahead */
#define DLA 2                   /* df_lookahead */
#define LA 2                    /* max(conv_lookahead, df_lookahead) */
#define CH 64                   /* conv_ch */
#define EMB 512                 /* conv_ch * nb_erb / 4 */
#define HID 256                 /* emb_hidden_dim = df_hidden_dim */
#define KTP 5                   /* df_pathway_kernel_size_t */
#define NY (ORD + CLA)          /* rolling_spec_buf_y length */
#define NX (ORD > LA ? ORD : LA) /* rolling_spec_buf_x length */
#define MIN_NB_ERB_FREQS 2      /* config.ini [df] min_nb_erb_freqs */
#define NORM_TAU 1.0f           /* config.ini [df] norm_tau */

typedef struct { float re, im; } cf;
typedef struct { double re, im; } cd;

typedef struct {                /* one GRU layer, weights transposed */
    float *wihT, *whhT;         /* [in][3H], [H][3H] */
    float *bih, *bhh;           /* [3H] */
} gru_l;

typedef struct { float *s, *b; } bnf; /* folded BatchNorm: y = x*s + b */

struct dfn3 {
    /* weights */
    float *erb_conv0_w; bnf erb_conv0_bn;                     /* [64,1,3,3] */
    float *erb_dw[3], *erb_pw[3]; bnf erb_bn[3];             /* erb_conv1..3 */
    float *df_conv0_w, *df_conv0_pw; bnf df_conv0_bn;         /* [64,1,3,3], [64,64] */
    float *df_conv1_dw, *df_conv1_pw; bnf df_conv1_bn;
    float *df_fc_emb;                                         /* [32,96,16] */
    float *enc_lin_in, *enc_lin_out; gru_l enc_gru;           /* [16,32,16], [16,16,32] */
    float *lsnr_w, lsnr_b;                                    /* [1,512] */
    float *erbd_lin_in, *erbd_lin_out; gru_l erbd_gru[2];
    float *convp_w[4]; bnf convp_bn[4];                       /* conv0p..conv3p [64] */
    float *convt_dw[3], *convt_pw[3]; bnf convt_bn[3];        /* convt1..3 */
    float *conv0_out_w; float conv0_out_s, conv0_out_b;       /* [1,64,1,3] */
    float *dfp_w, *dfp_pw; bnf dfp_bn;                        /* [10,32,5,1], [10,10] */
    float *dfd_lin_in; gru_l dfd_gru[2];                      /* [8,64,32] */
    float *df_skip, *df_out;                                  /* [16,32,16], [16,16,60] */
    float *allocs[256]; int nallocs;
    /* config-derived */
    float lsnr_min, lsnr_max, alpha;
    int erb[NE];
    float window[FFT], wnorm;
    cd tw[FFT];
    /* runtime parameters */
    int has_lim; float lim;
    int post_filter; float pf_beta;
    float min_db, max_erb_db, max_df_db;
    /* state */
    float ana_mem[FFT - HOP], syn_mem[FFT - HOP];
    float mean_norm[NE], unit_norm[ND];
    cf ybuf[NY][NF], xbuf[NX][NF];
    float erb_hist[2][NE];          /* feat_erb at t-2, t-1 */
    float cpl_hist[2][2][ND];       /* feat_spec (re, im) at t-2, t-1 */
    float enc_h[HID], erbd_h[2][HID], dfd_h[2][HID];
    float c0_hist[KTP - 1][CH * ND]; /* c0 at t-4..t-1 (df decoder) */
    /* scratch */
    cf spec[NF], out_spec[NF];
    float erbf[NE]; cf cplf[ND];
    float e0[CH * NE], e1[CH * NE / 2], e2[CH * NE / 4], e3[CH * NE / 4];
    float c0[CH * ND], c1[CH * ND / 2], tmp[CH * ND], tmp2[CH * ND];
    float emb[EMB], v512[EMB], v512b[EMB], h256[HID], gi[3 * HID], gh[3 * HID];
    float mask[NE], coefs[ND * ORD * 2], dfo[ND * ORD * 2], pth[2 * ORD * ND];
    float frame[FFT];
    cd fa[FFT], fb[FFT];
};

/* ---------- safetensors ---------- */

typedef struct {
    unsigned char *buf; size_t len;
    char *hdr; const unsigned char *data; size_t dlen;
    int ntensors, used;
} stf;

static int st_open(stf *f, const char *path, char *err, int el) {
    memset(f, 0, sizeof *f);
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, el, "cannot open %s", path); return -1; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 8) { fclose(fp); snprintf(err, el, "%s: too short", path); return -1; }
    f->buf = malloc(n);
    f->len = n;
    if (!f->buf || fread(f->buf, 1, n, fp) != (size_t)n) {
        fclose(fp); snprintf(err, el, "%s: read failed", path); return -1;
    }
    fclose(fp);
    uint64_t hl = 0;
    for (int i = 7; i >= 0; i--) hl = hl << 8 | f->buf[i];
    if (hl > f->len - 8) { snprintf(err, el, "%s: bad header length", path); return -1; }
    f->hdr = malloc(hl + 1);
    memcpy(f->hdr, f->buf + 8, hl);
    f->hdr[hl] = 0;
    f->data = f->buf + 8 + hl;
    f->dlen = f->len - 8 - hl;
    for (const char *p = f->hdr; (p = strstr(p, "\"data_offsets\"")); p++) f->ntensors++;
    return 0;
}

static void st_close(stf *f) { free(f->buf); free(f->hdr); }

/* Finds "name":{...} as a key of the top-level object. */
static const char *st_find(const stf *f, const char *name, const char **end) {
    char key[256];
    snprintf(key, sizeof key, "\"%s\"", name);
    size_t kl = strlen(key);
    for (const char *p = f->hdr; (p = strstr(p, key)); p += kl) {
        const char *b = p;
        while (b > f->hdr && (b[-1] == ' ' || b[-1] == '\n' || b[-1] == '\t')) b--;
        if (b != f->hdr && b[-1] != '{' && b[-1] != ',') continue;
        const char *q = p + kl;
        while (*q == ' ' || *q == '\n' || *q == '\t') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\n' || *q == '\t') q++;
        if (*q != '{') continue;
        const char *e = strchr(q, '}');
        if (!e) return NULL;
        *end = e;
        return q;
    }
    return NULL;
}

static const char *fld(const char *p, const char *end, const char *name) {
    char key[64];
    snprintf(key, sizeof key, "\"%s\"", name);
    const char *q = strstr(p, key);
    if (!q || q >= end) return NULL;
    q += strlen(key);
    while (*q == ' ' || *q == ':') q++;
    return q;
}

static float *st_get(dfn3 *d, stf *f, const char *name, int nd, int d0, int d1, int d2, int d3,
                     char *err, int el) {
    const char *end, *p = st_find(f, name, &end);
    if (!p) { snprintf(err, el, "tensor %s missing", name); return NULL; }
    const char *t = fld(p, end, "dtype");
    if (!t || strncmp(t, "\"F32\"", 5)) { snprintf(err, el, "tensor %s: dtype not F32", name); return NULL; }
    int want[4] = {d0, d1, d2, d3}, got[8], ng = 0;
    const char *s = fld(p, end, "shape");
    if (!s || *s != '[') { snprintf(err, el, "tensor %s: no shape", name); return NULL; }
    s++;
    while (*s && *s != ']' && ng < 8) {
        char *e;
        got[ng++] = (int)strtol(s, &e, 10);
        s = e;
        while (*s == ',' || *s == ' ') s++;
    }
    int ok = ng == nd;
    for (int i = 0; ok && i < nd; i++) ok = got[i] == want[i];
    if (!ok) {
        snprintf(err, el, "tensor %s: shape mismatch (%d dims, first %d)", name, ng, ng ? got[0] : -1);
        return NULL;
    }
    const char *o = fld(p, end, "data_offsets");
    if (!o || *o != '[') { snprintf(err, el, "tensor %s: no data_offsets", name); return NULL; }
    char *e;
    unsigned long long b0 = strtoull(o + 1, &e, 10);
    while (*e == ',' || *e == ' ') e++;
    unsigned long long b1 = strtoull(e, &e, 10);
    size_t n = 1;
    for (int i = 0; i < nd; i++) n *= want[i];
    if (b1 < b0 || b1 > f->dlen || b1 - b0 != n * 4) {
        snprintf(err, el, "tensor %s: bad data_offsets", name); return NULL;
    }
    float *w = malloc(n * 4);
    memcpy(w, f->data + b0, n * 4);    /* data region is not 4-byte aligned */
    d->allocs[d->nallocs++] = w;
    f->used++;
    return w;
}

static float *keep(dfn3 *d, size_t n) {
    float *w = calloc(n, sizeof(float));
    d->allocs[d->nallocs++] = w;
    return w;
}

/* BatchNorm2d in eval mode, folded to a scale and shift. */
static int ld_bn(dfn3 *d, stf *f, const char *pre, int n, bnf *o, char *err, int el) {
    char k[128];
    snprintf(k, sizeof k, "%s.weight", pre);
    float *g = st_get(d, f, k, 1, n, 0, 0, 0, err, el);
    snprintf(k, sizeof k, "%s.bias", pre);
    float *b = st_get(d, f, k, 1, n, 0, 0, 0, err, el);
    snprintf(k, sizeof k, "%s.running_mean", pre);
    float *m = st_get(d, f, k, 1, n, 0, 0, 0, err, el);
    snprintf(k, sizeof k, "%s.running_var", pre);
    float *v = st_get(d, f, k, 1, n, 0, 0, 0, err, el);
    if (!g || !b || !m || !v) return -1;
    o->s = keep(d, n);
    o->b = keep(d, n);
    for (int i = 0; i < n; i++) {
        o->s[i] = g[i] / sqrtf(v[i] + 1e-5f);
        o->b[i] = b[i] - m[i] * o->s[i];
    }
    return 0;
}

static int ld_gru(dfn3 *d, stf *f, const char *pre, int layer, gru_l *L, char *err, int el) {
    char k[128];
    snprintf(k, sizeof k, "%s.weight_ih_l%d", pre, layer);
    float *wih = st_get(d, f, k, 2, 3 * HID, HID, 0, 0, err, el);
    snprintf(k, sizeof k, "%s.weight_hh_l%d", pre, layer);
    float *whh = st_get(d, f, k, 2, 3 * HID, HID, 0, 0, err, el);
    snprintf(k, sizeof k, "%s.bias_ih_l%d", pre, layer);
    L->bih = st_get(d, f, k, 1, 3 * HID, 0, 0, 0, err, el);
    snprintf(k, sizeof k, "%s.bias_hh_l%d", pre, layer);
    L->bhh = st_get(d, f, k, 1, 3 * HID, 0, 0, 0, err, el);
    if (!wih || !whh || !L->bih || !L->bhh) return -1;
    L->wihT = keep(d, 3 * HID * HID);
    L->whhT = keep(d, 3 * HID * HID);
    for (int r = 0; r < 3 * HID; r++)
        for (int i = 0; i < HID; i++) {
            L->wihT[i * 3 * HID + r] = wih[r * HID + i];
            L->whhT[i * 3 * HID + r] = whh[r * HID + i];
        }
    return 0;
}

/* ---------- config.json ---------- */

static const char *jkey(const char *js, const char *key) {
    char k[96];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p) return NULL;
    p += strlen(k);
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ':') p++;
    return p;
}

static int check_config(const char *path, float *lmin, float *lmax, char *err, int el) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;                 /* no config: shapes are still checked */
    char js[16384];
    size_t n = fread(js, 1, sizeof js - 1, fp);
    fclose(fp);
    js[n] = 0;
    static const struct { const char *k; double v; } num[] = {
        {"sample_rate", SR}, {"fft_size", FFT}, {"hop_size", HOP}, {"nb_erb", NE},
        {"nb_df", ND}, {"df_order", ORD}, {"df_lookahead", DLA}, {"conv_lookahead", CLA},
        {"conv_ch", CH}, {"emb_hidden_dim", HID}, {"emb_num_layers", 3},
        {"df_hidden_dim", HID}, {"df_num_layers", 2}, {"linear_groups", 16},
        {"enc_linear_groups", 32}, {"conv_k_enc", 1}, {"conv_k_dec", 1},
        {"conv_width_factor", 1}};
    for (unsigned i = 0; i < sizeof num / sizeof num[0]; i++) {
        const char *p = jkey(js, num[i].k);
        if (!p || strtod(p, NULL) != num[i].v) {
            snprintf(err, el, "config %s: %s is not %g", path, num[i].k, num[i].v);
            return -1;
        }
    }
    static const struct { const char *k, *v; } lit[] = {
        {"conv_dec_mode", "\"transposed\""}, {"group_shuffle", "false"}, {"mask_pf", "false"},
        {"conv_depthwise", "true"}, {"convt_depthwise", "false"}, {"enc_concat", "false"},
        {"emb_gru_skip_enc", "\"none\""}, {"emb_gru_skip", "\"none\""},
        {"df_gru_skip", "\"groupedlinear\""}, {"dfop_method", "\"df\""},
        {"model_version", "\"DeepFilterNet3\""}};
    for (unsigned i = 0; i < sizeof lit / sizeof lit[0]; i++) {
        const char *p = jkey(js, lit[i].k);
        if (!p || strncmp(p, lit[i].v, strlen(lit[i].v))) {
            snprintf(err, el, "config %s: %s is not %s", path, lit[i].k, lit[i].v);
            return -1;
        }
    }
    static const struct { const char *k; int a, b; } arr[] = {
        {"conv_kernel", 1, 3}, {"convt_kernel", 1, 3}, {"conv_kernel_inp", 3, 3}};
    for (unsigned i = 0; i < sizeof arr / sizeof arr[0]; i++) {
        const char *p = jkey(js, arr[i].k);
        char *e;
        if (!p || *p != '[') goto bad;
        long a = strtol(p + 1, &e, 10);
        while (*e == ',' || *e == ' ' || *e == '\n') e++;
        long b = strtol(e, NULL, 10);
        if (a != arr[i].a || b != arr[i].b) goto bad;
        continue;
    bad:
        snprintf(err, el, "config %s: %s is not [%d, %d]", path, arr[i].k, arr[i].a, arr[i].b);
        return -1;
    }
    const char *p = jkey(js, "lsnr_min"), *q = jkey(js, "lsnr_max");
    if (!p || !q) { snprintf(err, el, "config %s: no lsnr range", path); return -1; }
    *lmin = strtof(p, NULL);
    *lmax = strtof(q, NULL);
    return 0;
}

/* ---------- DSP (libDF/src/lib.rs) ---------- */

static float freq2erb(float hz) { return 9.265f * log1pf(hz / (24.7f * 9.265f)); }
static float erb2freq(float e) { return 24.7f * 9.265f * (expf(e / 9.265f) - 1.f); }

static void erb_widths(int *erb) {
    float fw = (float)SR / (float)FFT;
    float lo = freq2erb(0.f), hi = freq2erb((float)(SR / 2));
    float step = (hi - lo) / NE;
    int prev = 0, over = 0, sum = 0;
    for (int i = 1; i <= NE; i++) {
        float f = erb2freq(lo + (float)i * step);
        int fb = (int)roundf(f / fw);
        int nb = fb - prev - over;
        if (nb < MIN_NB_ERB_FREQS) { over = MIN_NB_ERB_FREQS - nb; nb = MIN_NB_ERB_FREQS; }
        else over = 0;
        erb[i - 1] = nb;
        prev = fb;
    }
    erb[NE - 1] += 1;
    for (int i = 0; i < NE; i++) sum += erb[i];
    if (sum > NF) erb[NE - 1] -= sum - NF;
}

/* calc_norm_alpha: exp(-hop/sr/tau), rounded to the first precision < 1 */
static float norm_alpha(void) {
    float dt = (float)HOP / (float)SR;
    float al = expf(-dt / NORM_TAU), a = 1.f;
    int prec = 3;
    while (a >= 1.f) {
        float p10 = (float)(int)pow(10, prec);
        a = roundf(al * p10) / p10;
        prec++;
    }
    return a;
}

/* Mixed-radix complex FFT, N = 960 = 4*4*4*3*5, double precision. */
static const int fft_fac[] = {4, 4, 4, 3, 5};

static void fft_rec(const cd *tw, const cd *in, int stride, cd *out, int n, const int *fac,
                    int tws, int inv) {
    if (n == 1) { out[0] = in[0]; return; }
    int p = fac[0], m = n / p;
    for (int j = 0; j < p; j++)
        fft_rec(tw, in + j * stride, stride * p, out + j * m, m, fac + 1, tws * p, inv);
    cd t[5];
    for (int k = 0; k < m; k++) {
        for (int j = 0; j < p; j++) t[j] = out[j * m + k];
        for (int q = 0; q < p; q++) {
            int idx = k + q * m;
            double re = 0, im = 0;
            for (int j = 0; j < p; j++) {
                cd w = tw[(long)j * idx * tws % FFT];
                if (inv) w.im = -w.im;
                re += t[j].re * w.re - t[j].im * w.im;
                im += t[j].re * w.im + t[j].im * w.re;
            }
            out[idx].re = re;
            out[idx].im = im;
        }
    }
}

/* frame_analysis: window over [previous hop, this hop], rfft, * wnorm */
static void analysis(dfn3 *d, const float *in, cf *out) {
    for (int i = 0; i < FFT - HOP; i++) d->frame[i] = d->ana_mem[i] * d->window[i];
    for (int i = 0; i < HOP; i++) d->frame[FFT - HOP + i] = in[i] * d->window[FFT - HOP + i];
    memcpy(d->ana_mem, in, sizeof(float) * HOP);
    for (int i = 0; i < FFT; i++) { d->fa[i].re = d->frame[i]; d->fa[i].im = 0; }
    fft_rec(d->tw, d->fa, 1, d->fb, FFT, fft_fac, 1, 0);
    for (int k = 0; k < NF; k++) {
        out[k].re = (float)d->fb[k].re * d->wnorm;
        out[k].im = (float)d->fb[k].im * d->wnorm;
    }
}

/* frame_synthesis: unnormalised irfft (realfft drops the imaginary parts
 * of DC and Nyquist), window, overlap-add. */
static void synthesis(dfn3 *d, const cf *in, float *out) {
    for (int k = 0; k < NF; k++) { d->fa[k].re = in[k].re; d->fa[k].im = in[k].im; }
    d->fa[0].im = 0;
    d->fa[NF - 1].im = 0;
    for (int k = NF; k < FFT; k++) { d->fa[k].re = in[FFT - k].re; d->fa[k].im = -in[FFT - k].im; }
    fft_rec(d->tw, d->fa, 1, d->fb, FFT, fft_fac, 1, 1);
    for (int i = 0; i < FFT; i++) d->frame[i] = (float)d->fb[i].re * d->window[i];
    for (int i = 0; i < HOP; i++) out[i] = d->frame[i] + d->syn_mem[i];
    memcpy(d->syn_mem, d->frame + HOP, sizeof(float) * (FFT - HOP));
}

static void feat_erb(dfn3 *d, const cf *x, float *o) {
    int b0 = 0;
    for (int b = 0; b < NE; b++) {
        float k = 1.f / (float)d->erb[b], acc = 0;
        for (int j = 0; j < d->erb[b]; j++) {
            const cf *v = &x[b0 + j];
            acc += (v->re * v->re + v->im * v->im) * k;
        }
        b0 += d->erb[b];
        float y = log10f(acc + 1e-10f) * 10.f;
        float *s = &d->mean_norm[b];
        *s = y * (1.f - d->alpha) + *s * d->alpha;
        y -= *s;
        o[b] = y / 40.f;
    }
}

static void feat_cplx(dfn3 *d, const cf *x, cf *o) {
    for (int f = 0; f < ND; f++) {
        float *s = &d->unit_norm[f];
        *s = hypotf(x[f].re, x[f].im) * (1.f - d->alpha) + *s * d->alpha;
        float r = sqrtf(*s);
        o[f].re = x[f].re / r;
        o[f].im = x[f].im / r;
    }
}

static void post_filter(const cf *noisy, cf *enh, float beta) {
    const float eps = 1e-12f, pi = 3.14159265358979323846f;
    for (int i = 0; i + 4 <= NF; i += 4)          /* chunks_exact(4): bin 480 untouched */
        for (int j = i; j < i + 4; j++) {
            float g = hypotf(enh[j].re, enh[j].im) / (hypotf(noisy[j].re, noisy[j].im) + eps);
            g = fmaxf(fminf(g, 1.f), eps);
            float gs = g * sinf(g * pi / 2.f);
            float q = g / gs;
            float pf = ((beta + 1.f) * g / (1.f + beta * q * q)) / g;
            enh[j].re *= pf;
            enh[j].im *= pf;
        }
}

/* ---------- network (df/deepfilternet3.py, df/modules.py) ---------- */

static inline float relu(float x) { return x > 0.f ? x : 0.f; }
static inline float sigm(float x) { return 1.f / (1.f + expf(-x)); }

static void axpy(float *y, const float *x, float a, int n) {
    for (int i = 0; i < n; i++) y[i] += a * x[i];
}

/* GroupedLinearEinsum: y[g*O+h] = sum_i x[g*I+i] W[g][i][h] */
static void glin(const float *x, int in, int out, int g, const float *W, float *y) {
    int I = in / g, O = out / g;
    memset(y, 0, sizeof(float) * out);
    for (int k = 0; k < g; k++)
        for (int i = 0; i < I; i++) axpy(y + k * O, W + (size_t)(k * I + i) * O, x[k * I + i], O);
}

/* nn.GRU, one step: r,z,n gates, b_hn inside the reset product */
static void gru(dfn3 *d, const gru_l *L, const float *x, float *h) {
    memcpy(d->gi, L->bih, sizeof(float) * 3 * HID);
    memcpy(d->gh, L->bhh, sizeof(float) * 3 * HID);
    for (int i = 0; i < HID; i++) axpy(d->gi, L->wihT + (size_t)i * 3 * HID, x[i], 3 * HID);
    for (int i = 0; i < HID; i++) axpy(d->gh, L->whhT + (size_t)i * 3 * HID, h[i], 3 * HID);
    for (int k = 0; k < HID; k++) {
        float r = sigm(d->gi[k] + d->gh[k]);
        float z = sigm(d->gi[HID + k] + d->gh[HID + k]);
        float n = tanhf(d->gi[2 * HID + k] + r * d->gh[2 * HID + k]);
        h[k] = (1.f - z) * n + z * h[k];
    }
}

/* depthwise (1,3) conv over frequency, padding 1, stride st */
static void dw3(const float *x, int fi, int st, const float *w, float *y, int fo) {
    for (int c = 0; c < CH; c++)
        for (int f = 0; f < fo; f++) {
            float acc = 0;
            for (int j = 0; j < 3; j++) {
                int i = f * st - 1 + j;
                if (i >= 0 && i < fi) acc += w[c * 3 + j] * x[c * fi + i];
            }
            y[c * fo + f] = acc;
        }
}

/* depthwise (1,3) ConvTranspose2d, stride 2, padding 1, output_padding 1:
 * out[2i-1+k] += x[i] w[k], fo = 2 fi */
static void dwt3(const float *x, int fi, const float *w, float *y) {
    int fo = 2 * fi;
    memset(y, 0, sizeof(float) * CH * fo);
    for (int c = 0; c < CH; c++)
        for (int i = 0; i < fi; i++)
            for (int k = 0; k < 3; k++) {
                int o = 2 * i - 1 + k;
                if (o >= 0 && o < fo) y[c * fo + o] += x[c * fi + i] * w[c * 3 + k];
            }
}

/* 1x1 conv: y[o][f] = sum_c W[o][c] x[c][f] */
static void pw(const float *x, int ci, int nf, const float *W, int co, float *y) {
    memset(y, 0, sizeof(float) * co * nf);
    for (int o = 0; o < co; o++)
        for (int c = 0; c < ci; c++) axpy(y + o * nf, x + c * nf, W[o * ci + c], nf);
}

static void bn_relu(float *y, int c, int nf, const bnf *b) {
    for (int k = 0; k < c; k++)
        for (int f = 0; f < nf; f++) y[k * nf + f] = relu(y[k * nf + f] * b->s[k] + b->b[k]);
}

static float encoder(dfn3 *d) {
    /* erb_conv0: [64,1,3,3], time taps t-2,t-1,t; freq padding 1 */
    const float *ef[3] = {d->erb_hist[0], d->erb_hist[1], d->erbf};
    for (int c = 0; c < CH; c++)
        for (int f = 0; f < NE; f++) {
            float acc = 0;
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 3; j++) {
                    int i = f - 1 + j;
                    if (i >= 0 && i < NE) acc += d->erb_conv0_w[(c * 3 + k) * 3 + j] * ef[k][i];
                }
            d->e0[c * NE + f] = acc;
        }
    bn_relu(d->e0, CH, NE, &d->erb_conv0_bn);
    dw3(d->e0, NE, 2, d->erb_dw[0], d->tmp, NE / 2);
    pw(d->tmp, CH, NE / 2, d->erb_pw[0], CH, d->e1);
    bn_relu(d->e1, CH, NE / 2, &d->erb_bn[0]);
    dw3(d->e1, NE / 2, 2, d->erb_dw[1], d->tmp, NE / 4);
    pw(d->tmp, CH, NE / 4, d->erb_pw[1], CH, d->e2);
    bn_relu(d->e2, CH, NE / 4, &d->erb_bn[1]);
    dw3(d->e2, NE / 4, 1, d->erb_dw[2], d->tmp, NE / 4);
    pw(d->tmp, CH, NE / 4, d->erb_pw[2], CH, d->e3);
    bn_relu(d->e3, CH, NE / 4, &d->erb_bn[2]);

    /* df_conv0: groups 2 (real -> 0..31, imag -> 32..63), then 1x1 */
    float cur[2][ND];
    for (int f = 0; f < ND; f++) { cur[0][f] = d->cplf[f].re; cur[1][f] = d->cplf[f].im; }
    for (int c = 0; c < CH; c++) {
        int g = c / (CH / 2);
        const float *xf[3] = {d->cpl_hist[0][g], d->cpl_hist[1][g], cur[g]};
        for (int f = 0; f < ND; f++) {
            float acc = 0;
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 3; j++) {
                    int i = f - 1 + j;
                    if (i >= 0 && i < ND) acc += d->df_conv0_w[(c * 3 + k) * 3 + j] * xf[k][i];
                }
            d->tmp[c * ND + f] = acc;
        }
    }
    pw(d->tmp, CH, ND, d->df_conv0_pw, CH, d->c0);
    bn_relu(d->c0, CH, ND, &d->df_conv0_bn);
    dw3(d->c0, ND, 2, d->df_conv1_dw, d->tmp, ND / 2);
    pw(d->tmp, CH, ND / 2, d->df_conv1_pw, CH, d->c1);
    bn_relu(d->c1, CH, ND / 2, &d->df_conv1_bn);

    /* conv time histories advance */
    memcpy(d->erb_hist[0], d->erb_hist[1], sizeof d->erb_hist[0]);
    memcpy(d->erb_hist[1], d->erbf, sizeof d->erb_hist[1]);
    memcpy(d->cpl_hist[0], d->cpl_hist[1], sizeof d->cpl_hist[0]);
    memcpy(d->cpl_hist[1], cur, sizeof d->cpl_hist[1]);

    /* cemb = relu(df_fc_emb(c1 as [F][C])); emb = e3 as [F][C] + cemb */
    for (int f = 0; f < ND / 2; f++)
        for (int c = 0; c < CH; c++) d->tmp2[f * CH + c] = d->c1[c * (ND / 2) + f];
    glin(d->tmp2, CH * ND / 2, EMB, 32, d->df_fc_emb, d->v512);
    for (int f = 0; f < NE / 4; f++)
        for (int c = 0; c < CH; c++)
            d->v512b[f * CH + c] = d->e3[c * (NE / 4) + f] + relu(d->v512[f * CH + c]);

    /* emb_gru: SqueezedGRU_S(512, 256, out 512, 1 layer, groups 16, ReLU) */
    glin(d->v512b, EMB, HID, 16, d->enc_lin_in, d->h256);
    for (int i = 0; i < HID; i++) d->h256[i] = relu(d->h256[i]);
    gru(d, &d->enc_gru, d->h256, d->enc_h);
    glin(d->enc_h, HID, EMB, 16, d->enc_lin_out, d->emb);
    for (int i = 0; i < EMB; i++) d->emb[i] = relu(d->emb[i]);

    float acc = d->lsnr_b;
    for (int i = 0; i < EMB; i++) acc += d->lsnr_w[i] * d->emb[i];
    return sigm(acc) * (d->lsnr_max - d->lsnr_min) + d->lsnr_min;
}

/* 1x1 depthwise pathway conv + BN + ReLU, added to y */
static void convp_add(const float *x, int nf, const float *w, const bnf *b, float *y) {
    for (int c = 0; c < CH; c++)
        for (int f = 0; f < nf; f++)
            y[c * nf + f] += relu(x[c * nf + f] * w[c] * b->s[c] + b->b[c]);
}

static void erb_decoder(dfn3 *d) {
    glin(d->emb, EMB, HID, 16, d->erbd_lin_in, d->h256);
    for (int i = 0; i < HID; i++) d->h256[i] = relu(d->h256[i]);
    gru(d, &d->erbd_gru[0], d->h256, d->erbd_h[0]);
    gru(d, &d->erbd_gru[1], d->erbd_h[0], d->erbd_h[1]);
    glin(d->erbd_h[1], HID, EMB, 16, d->erbd_lin_out, d->v512);
    /* emb.view(t, 8, 64).permute -> [64][8], plus conv3p(e3) */
    float *x = d->tmp2, *y = d->tmp;
    for (int f = 0; f < NE / 4; f++)
        for (int c = 0; c < CH; c++) x[c * (NE / 4) + f] = relu(d->v512[f * CH + c]);
    convp_add(d->e3, NE / 4, d->convp_w[3], &d->convp_bn[3], x);
    /* convt3: (1,3) conv, stride 1 */
    float t[CH * NE];
    dw3(x, NE / 4, 1, d->convt_dw[2], y, NE / 4);
    pw(y, CH, NE / 4, d->convt_pw[2], CH, t);
    bn_relu(t, CH, NE / 4, &d->convt_bn[2]);
    /* convt2: transposed, 8 -> 16 */
    convp_add(d->e2, NE / 4, d->convp_w[2], &d->convp_bn[2], t);
    dwt3(t, NE / 4, d->convt_dw[1], y);
    pw(y, CH, NE / 2, d->convt_pw[1], CH, x);
    bn_relu(x, CH, NE / 2, &d->convt_bn[1]);
    /* convt1: transposed, 16 -> 32 */
    convp_add(d->e1, NE / 2, d->convp_w[1], &d->convp_bn[1], x);
    dwt3(x, NE / 2, d->convt_dw[0], y);
    pw(y, CH, NE, d->convt_pw[0], CH, t);
    bn_relu(t, CH, NE, &d->convt_bn[0]);
    /* conv0_out: (1,3) conv 64 -> 1, BN, sigmoid */
    convp_add(d->e0, NE, d->convp_w[0], &d->convp_bn[0], t);
    for (int f = 0; f < NE; f++) {
        float acc = 0;
        for (int c = 0; c < CH; c++)
            for (int j = 0; j < 3; j++) {
                int i = f - 1 + j;
                if (i >= 0 && i < NE) acc += d->conv0_out_w[c * 3 + j] * t[c * NE + i];
            }
        d->mask[f] = sigm(acc * d->conv0_out_s + d->conv0_out_b);
    }
}

static void df_decoder(dfn3 *d) {
    glin(d->emb, EMB, HID, 8, d->dfd_lin_in, d->h256);
    for (int i = 0; i < HID; i++) d->h256[i] = relu(d->h256[i]);
    gru(d, &d->dfd_gru[0], d->h256, d->dfd_h[0]);
    gru(d, &d->dfd_gru[1], d->dfd_h[0], d->dfd_h[1]);
    float c[HID];
    glin(d->emb, EMB, HID, 16, d->df_skip, c);
    for (int i = 0; i < HID; i++) c[i] += d->dfd_h[1][i];
    glin(c, HID, ND * ORD * 2, 16, d->df_out, d->dfo);
    /* df_convp: (5,1) conv, groups 2 (in 0..31 -> out 0..4, 32..63 -> 5..9), 1x1, BN, ReLU */
    const float *cf_[KTP];
    for (int k = 0; k < KTP - 1; k++) cf_[k] = d->c0_hist[k];
    cf_[KTP - 1] = d->c0;
    float *p1 = d->tmp;
    for (int o = 0; o < 2 * ORD; o++) {
        int g = o / ORD;
        float *y = p1 + o * ND;
        memset(y, 0, sizeof(float) * ND);
        for (int ci = 0; ci < CH / 2; ci++)
            for (int k = 0; k < KTP; k++)
                axpy(y, cf_[k] + (g * (CH / 2) + ci) * ND, d->dfp_w[(o * (CH / 2) + ci) * KTP + k], ND);
    }
    memset(d->pth, 0, sizeof d->pth);
    for (int o = 0; o < 2 * ORD; o++)
        for (int c2 = 0; c2 < 2 * ORD; c2++) axpy(d->pth + o * ND, p1 + c2 * ND, d->dfp_pw[o * 2 * ORD + c2], ND);
    for (int o = 0; o < 2 * ORD; o++)
        for (int f = 0; f < ND; f++)
            d->pth[o * ND + f] = relu(d->pth[o * ND + f] * d->dfp_bn.s[o] + d->dfp_bn.b[o]);
    for (int k = 0; k < KTP - 2; k++) memcpy(d->c0_hist[k], d->c0_hist[k + 1], sizeof d->c0_hist[0]);
    memcpy(d->c0_hist[KTP - 2], d->c0, sizeof d->c0_hist[0]);
    /* coefs[f][o*2+ri] = tanh(df_out)[f*10 + j] + df_convp[j][f] */
    for (int f = 0; f < ND; f++)
        for (int j = 0; j < 2 * ORD; j++)
            d->coefs[f * 2 * ORD + j] = tanhf(d->dfo[f * 2 * ORD + j]) + d->pth[j * ND + f];
}

/* ---------- streaming loop (tract.rs DfTract::process) ---------- */

float dfn3_process(dfn3 *d, const float *in, float *out) {
    float e = 0;
    for (int i = 0; i < HOP; i++) e += in[i] * in[i];
    if (e / (float)HOP < 1e-7f) {           /* silence: nothing advances */
        memset(out, 0, sizeof(float) * HOP);
        return -15.f;
    }
    memmove(d->ybuf[0], d->ybuf[1], sizeof(cf) * NF * (NY - 1));
    memmove(d->xbuf[0], d->xbuf[1], sizeof(cf) * NF * (NX - 1));
    analysis(d, in, d->spec);
    memcpy(d->ybuf[NY - 1], d->spec, sizeof d->spec);
    memcpy(d->xbuf[NX - 1], d->spec, sizeof d->spec);
    if (d->has_lim && d->lim == 1.f) {       /* atten limit < 0.01 dB */
        if (out != in) memcpy(out, in, sizeof(float) * HOP);
        return 35.f;
    }
    feat_erb(d, d->spec, d->erbf);
    feat_cplx(d, d->spec, d->cplf);
    float lsnr = encoder(d);
    int gains = 0, zeros = 0, df = 0;        /* apply_stages */
    if (lsnr < d->min_db) zeros = 1;
    else if (lsnr > d->max_erb_db) ;
    else if (lsnr > d->max_df_db) gains = 1;
    else gains = df = 1;
    if (gains) erb_decoder(d);
    if (df) df_decoder(d);

    cf *ys = d->ybuf[ORD - 1];               /* frame t - conv_lookahead */
    if (gains || zeros) {
        int b0 = 0;
        for (int b = 0; b < NE; b++) {
            float m = zeros ? 0.f : d->mask[b];
            for (int j = 0; j < d->erb[b]; j++) { ys[b0 + j].re *= m; ys[b0 + j].im *= m; }
            b0 += d->erb[b];
        }
    }
    memcpy(d->out_spec, ys, sizeof d->out_spec);
    if (df) {                                /* deep filter over frames t-4..t */
        for (int f = 0; f < ND; f++) { d->out_spec[f].re = 0; d->out_spec[f].im = 0; }
        for (int i = 0; i < ORD; i++)
            for (int f = 0; f < ND; f++) {
                const cf s = d->xbuf[i][f];
                float cr = d->coefs[f * 2 * ORD + i * 2], ci = d->coefs[f * 2 * ORD + i * 2 + 1];
                d->out_spec[f].re += s.re * cr - s.im * ci;
                d->out_spec[f].im += s.re * ci + s.im * cr;
            }
    }
    const cf *noisy = d->xbuf[NX - LA - 1];  /* frame t - lookahead */
    if (gains && d->post_filter) post_filter(noisy, d->out_spec, d->pf_beta);
    if (d->has_lim) {
        for (int f = 0; f < NF; f++) {
            d->out_spec[f].re = d->out_spec[f].re * (1.f - d->lim) + d->lim * noisy[f].re;
            d->out_spec[f].im = d->out_spec[f].im * (1.f - d->lim) + d->lim * noisy[f].im;
        }
    }
    synthesis(d, d->out_spec, out);
    return lsnr;
}

void dfn3_set_atten_lim(dfn3 *d, float db) {
    float l = fabsf(db);
    if (l >= 100.f) d->has_lim = 0;
    else if (l < 0.01f) { d->has_lim = 1; d->lim = 1.f; }
    else { d->has_lim = 1; d->lim = powf(10.f, -l / 20.f); }
}

void dfn3_set_pf_beta(dfn3 *d, float beta) {
    if (beta > 0.f) { d->pf_beta = beta; d->post_filter = 1; }
    else { d->pf_beta = beta < 0.f ? 0.f : beta; d->post_filter = 0; }
}

void dfn3_set_thresholds(dfn3 *d, float min_db, float max_erb_db, float max_df_db) {
    d->min_db = min_db;
    d->max_erb_db = max_erb_db;
    d->max_df_db = max_df_db;
}

void dfn3_reset(dfn3 *d) {
    memset(d->ana_mem, 0, sizeof d->ana_mem);
    memset(d->syn_mem, 0, sizeof d->syn_mem);
    for (int i = 0; i < NE; i++) d->mean_norm[i] = -60.f + (float)i * ((-90.f - -60.f) / (float)(NE - 1));
    for (int i = 0; i < ND; i++) d->unit_norm[i] = 0.001f + (float)i * ((0.0001f - 0.001f) / (float)(ND - 1));
    memset(d->ybuf, 0, sizeof d->ybuf);
    memset(d->xbuf, 0, sizeof d->xbuf);
    memset(d->erb_hist, 0, sizeof d->erb_hist);
    memset(d->cpl_hist, 0, sizeof d->cpl_hist);
    memset(d->enc_h, 0, sizeof d->enc_h);
    memset(d->erbd_h, 0, sizeof d->erbd_h);
    memset(d->dfd_h, 0, sizeof d->dfd_h);
    memset(d->c0_hist, 0, sizeof d->c0_hist);
}

void dfn3_free(dfn3 *d) {
    if (!d) return;
    for (int i = 0; i < d->nallocs; i++) free(d->allocs[i]);
    free(d);
}

dfn3 *dfn3_new(const char *model, const char *config, char *err, int el) {
    static const uint16_t one = 1;
    if (*(const uint8_t *)&one != 1) { snprintf(err, el, "big-endian host"); return NULL; }
    dfn3 *d = calloc(1, sizeof *d);
    if (!d) { snprintf(err, el, "out of memory"); return NULL; }
    d->lsnr_min = -15.f;
    d->lsnr_max = 35.f;
    if (config && check_config(config, &d->lsnr_min, &d->lsnr_max, err, el) < 0) {
        free(d);
        return NULL;
    }
    stf f;
    if (st_open(&f, model, err, el) < 0) { st_close(&f); free(d); return NULL; }
    char k[128];
    int ok = 1;
#define G(dst, name, nd, a, b, c, e) ok = ok && ((dst) = st_get(d, &f, name, nd, a, b, c, e, err, el)) != NULL
    G(d->erb_conv0_w, "enc.erb_conv0.1.weight", 4, CH, 1, 3, 3);
    ok = ok && ld_bn(d, &f, "enc.erb_conv0.2", CH, &d->erb_conv0_bn, err, el) == 0;
    for (int i = 0; i < 3 && ok; i++) {
        snprintf(k, sizeof k, "enc.erb_conv%d.0.weight", i + 1);
        G(d->erb_dw[i], k, 4, CH, 1, 1, 3);
        snprintf(k, sizeof k, "enc.erb_conv%d.1.weight", i + 1);
        G(d->erb_pw[i], k, 4, CH, CH, 1, 1);
        snprintf(k, sizeof k, "enc.erb_conv%d.2", i + 1);
        ok = ok && ld_bn(d, &f, k, CH, &d->erb_bn[i], err, el) == 0;
    }
    G(d->df_conv0_w, "enc.df_conv0.1.weight", 4, CH, 1, 3, 3);
    G(d->df_conv0_pw, "enc.df_conv0.2.weight", 4, CH, CH, 1, 1);
    ok = ok && ld_bn(d, &f, "enc.df_conv0.3", CH, &d->df_conv0_bn, err, el) == 0;
    G(d->df_conv1_dw, "enc.df_conv1.0.weight", 4, CH, 1, 1, 3);
    G(d->df_conv1_pw, "enc.df_conv1.1.weight", 4, CH, CH, 1, 1);
    ok = ok && ld_bn(d, &f, "enc.df_conv1.2", CH, &d->df_conv1_bn, err, el) == 0;
    G(d->df_fc_emb, "enc.df_fc_emb.0.weight", 3, 32, CH * ND / 2 / 32, EMB / 32, 0);
    G(d->enc_lin_in, "enc.emb_gru.linear_in.0.weight", 3, 16, EMB / 16, HID / 16, 0);
    G(d->enc_lin_out, "enc.emb_gru.linear_out.0.weight", 3, 16, HID / 16, EMB / 16, 0);
    ok = ok && ld_gru(d, &f, "enc.emb_gru.gru", 0, &d->enc_gru, err, el) == 0;
    float *lb = NULL;
    G(d->lsnr_w, "enc.lsnr_fc.0.weight", 2, 1, EMB, 0, 0);
    G(lb, "enc.lsnr_fc.0.bias", 1, 1, 0, 0, 0);
    if (ok) d->lsnr_b = lb[0];

    G(d->erbd_lin_in, "erb_dec.emb_gru.linear_in.0.weight", 3, 16, EMB / 16, HID / 16, 0);
    G(d->erbd_lin_out, "erb_dec.emb_gru.linear_out.0.weight", 3, 16, HID / 16, EMB / 16, 0);
    for (int l = 0; l < 2 && ok; l++) ok = ld_gru(d, &f, "erb_dec.emb_gru.gru", l, &d->erbd_gru[l], err, el) == 0;
    for (int i = 0; i < 4 && ok; i++) {
        snprintf(k, sizeof k, "erb_dec.conv%dp.0.weight", i);
        G(d->convp_w[i], k, 4, CH, 1, 1, 1);
        snprintf(k, sizeof k, "erb_dec.conv%dp.1", i);
        ok = ok && ld_bn(d, &f, k, CH, &d->convp_bn[i], err, el) == 0;
    }
    for (int i = 0; i < 3 && ok; i++) {
        snprintf(k, sizeof k, "erb_dec.convt%d.0.weight", i + 1);
        G(d->convt_dw[i], k, 4, CH, 1, 1, 3);
        snprintf(k, sizeof k, "erb_dec.convt%d.1.weight", i + 1);
        G(d->convt_pw[i], k, 4, CH, CH, 1, 1);
        snprintf(k, sizeof k, "erb_dec.convt%d.2", i + 1);
        ok = ok && ld_bn(d, &f, k, CH, &d->convt_bn[i], err, el) == 0;
    }
    G(d->conv0_out_w, "erb_dec.conv0_out.0.weight", 4, 1, CH, 1, 3);
    bnf ob;
    ok = ok && ld_bn(d, &f, "erb_dec.conv0_out.1", 1, &ob, err, el) == 0;
    if (ok) { d->conv0_out_s = ob.s[0]; d->conv0_out_b = ob.b[0]; }

    G(d->dfp_w, "df_dec.df_convp.1.weight", 4, 2 * ORD, CH / 2, KTP, 1);
    G(d->dfp_pw, "df_dec.df_convp.2.weight", 4, 2 * ORD, 2 * ORD, 1, 1);
    ok = ok && ld_bn(d, &f, "df_dec.df_convp.3", 2 * ORD, &d->dfp_bn, err, el) == 0;
    G(d->dfd_lin_in, "df_dec.df_gru.linear_in.0.weight", 3, 8, EMB / 8, HID / 8, 0);
    for (int l = 0; l < 2 && ok; l++) ok = ld_gru(d, &f, "df_dec.df_gru.gru", l, &d->dfd_gru[l], err, el) == 0;
    G(d->df_skip, "df_dec.df_skip.weight", 3, 16, EMB / 16, HID / 16, 0);
    G(d->df_out, "df_dec.df_out.0.weight", 3, 16, HID / 16, ND * 2 * ORD / 16, 0);
    /* df_fc_a is defined in DfDecoder but unused in forward(); the ERB
     * filterbanks are buffers, used here only to check the ERB widths. */
    float *fa_w = NULL, *fa_b = NULL, *fbk = NULL, *ifb = NULL;
    G(fa_w, "df_dec.df_fc_a.0.weight", 2, 1, HID, 0, 0);
    G(fa_b, "df_dec.df_fc_a.0.bias", 1, 1, 0, 0, 0);
    G(fbk, "erb_fb", 2, NF, NE, 0, 0);
    G(ifb, "mask.erb_inv_fb", 2, NE, NF, 0, 0);
#undef G
    (void)fa_w; (void)fa_b;
    if (ok && f.used != f.ntensors) {
        snprintf(err, el, "%s: %d tensors in file, %d used", model, f.ntensors, f.used);
        ok = 0;
    }
    erb_widths(d->erb);
    if (ok) {                                /* widths implied by erb_fb */
        int b0 = 0;
        for (int b = 0; b < NE && ok; b++) {
            int w = 0;
            for (int i = 0; i < NF; i++) w += fbk[i * NE + b] != 0.f;
            for (int i = b0; i < b0 + d->erb[b] && i < NF; i++) ok = ok && fbk[i * NE + b] != 0.f;
            if (w != d->erb[b] || !ok) {
                snprintf(err, el, "ERB band %d: width %d computed, %d in erb_fb", b, d->erb[b], w);
                ok = 0;
            }
            b0 += d->erb[b];
        }
    }
    st_close(&f);
    if (!ok) { dfn3_free(d); return NULL; }

    d->alpha = norm_alpha();
    for (int i = 0; i < FFT; i++) {
        double s = sin(0.5 * M_PI * (i + 0.5) / (double)(FFT / 2));
        d->window[i] = (float)sin(0.5 * M_PI * s * s);
        d->tw[i].re = cos(-2.0 * M_PI * i / FFT);
        d->tw[i].im = sin(-2.0 * M_PI * i / FFT);
    }
    d->wnorm = 1.f / ((float)(FFT * FFT) / (float)(2 * HOP));
    dfn3_set_atten_lim(d, 100.f);
    dfn3_set_pf_beta(d, 0.f);
    dfn3_set_thresholds(d, -15.f, 35.f, 35.f);
    dfn3_reset(d);
    return d;
}
