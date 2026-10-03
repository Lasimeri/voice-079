/* deep_filter_st_ladspa.c: LADSPA wrapper around dfn3.c, a drop-in for the
 * Rust deep-filter-ladspa 0.5.6 plugin: label deep_filter_mono, the same
 * eight ports in the same order with the same names, bounds and default
 * hints. Weights from $NRST_MODEL (default ~/tts079/nr-st/model.safetensors),
 * checked against config.json in the same directory ($NRST_CONFIG
 * overrides) when that file exists.
 * Differences from the Rust plugin, by design:
 *   - synchronous: each run() buffers input into 480-sample hops and
 *     processes them in place, so the latency is a fixed 480 samples (the
 *     Rust plugin's starting proc_delay) and never grows on underruns;
 *     "Min Processing Buffer (frames)" is accepted and ignored;
 *   - activate() resets all state (the Rust plugin keeps it);
 *   - mono only (no deep_filter_stereo); a sample rate other than 48 kHz
 *     makes instantiate() return NULL instead of panicking.
 * Build: see build.sh (gcc -O2 -shared -fPIC ... -lm). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dfn3.h"
#include "ladspa-abi.h"

enum { P_IN, P_OUT, P_ATTEN, P_MIN, P_MAXERB, P_MAXDF, P_MINBUF, P_PFBETA, NPORTS };

static const LADSPA_PortDescriptor port_desc[NPORTS] = {
    LADSPA_PORT_AUDIO | LADSPA_PORT_INPUT,   LADSPA_PORT_AUDIO | LADSPA_PORT_OUTPUT,
    LADSPA_PORT_CONTROL | LADSPA_PORT_INPUT, LADSPA_PORT_CONTROL | LADSPA_PORT_INPUT,
    LADSPA_PORT_CONTROL | LADSPA_PORT_INPUT, LADSPA_PORT_CONTROL | LADSPA_PORT_INPUT,
    LADSPA_PORT_CONTROL | LADSPA_PORT_INPUT, LADSPA_PORT_CONTROL | LADSPA_PORT_INPUT};

static const char *const port_name[NPORTS] = {
    "Audio In", "Audio Out", "Attenuation Limit (dB)", "Min processing threshold (dB)",
    "Max ERB processing threshold (dB)", "Max DF processing threshold (dB)",
    "Min Processing Buffer (frames)", "Post Filter Beta"};

#define BOUNDED (LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE)
static const LADSPA_PortRangeHint port_hint[NPORTS] = {
    {0, 0, 0},
    {0, 0, 0},
    {BOUNDED | LADSPA_HINT_DEFAULT_MAXIMUM, 0.f, 100.f},
    {BOUNDED | LADSPA_HINT_DEFAULT_MINIMUM, -15.f, 35.f},
    {BOUNDED | LADSPA_HINT_DEFAULT_MAXIMUM, -15.f, 35.f},
    {BOUNDED | LADSPA_HINT_DEFAULT_MAXIMUM, -15.f, 35.f},
    {BOUNDED | LADSPA_HINT_DEFAULT_MINIMUM, 0.f, 10.f},
    {BOUNDED | LADSPA_HINT_DEFAULT_MINIMUM, 0.f, 0.05f}};

/* value of an unconnected control port: its default hint */
static float port_default(int p) {
    const LADSPA_PortRangeHint *h = &port_hint[p];
    return (h->HintDescriptor & LADSPA_HINT_DEFAULT_MASK) == LADSPA_HINT_DEFAULT_MAXIMUM
               ? h->UpperBound : h->LowerBound;
}

typedef struct {
    dfn3 *m;
    LADSPA_Data *port[NPORTS];
    float cur[NPORTS];          /* control values in effect */
    float inq[DFN3_HOP], outq[DFN3_HOP];
    int pos;
} inst;

static void apply_controls(inst *s, int force) {
    float v[NPORTS];
    for (int p = P_ATTEN; p < NPORTS; p++) v[p] = s->port[p] ? *s->port[p] : port_default(p);
    if (force || v[P_ATTEN] != s->cur[P_ATTEN]) dfn3_set_atten_lim(s->m, v[P_ATTEN]);
    if (force || v[P_PFBETA] != s->cur[P_PFBETA]) dfn3_set_pf_beta(s->m, v[P_PFBETA]);
    if (force || v[P_MIN] != s->cur[P_MIN] || v[P_MAXERB] != s->cur[P_MAXERB] ||
        v[P_MAXDF] != s->cur[P_MAXDF])
        dfn3_set_thresholds(s->m, v[P_MIN], v[P_MAXERB], v[P_MAXDF]);
    for (int p = P_ATTEN; p < NPORTS; p++) s->cur[p] = v[p];
}

static LADSPA_Handle instantiate(const LADSPA_Descriptor *desc, unsigned long rate) {
    (void)desc;
    if (rate != DFN3_SR) {
        fprintf(stderr, "deep_filter_st: sample rate %lu unsupported (48000 only)\n", rate);
        return NULL;
    }
    char model[4096], config[4096], err[512];
    const char *e = getenv("NRST_MODEL");
    if (e && *e) snprintf(model, sizeof model, "%s", e);
    else snprintf(model, sizeof model, "%s/tts079/nr-st/model.safetensors", getenv("HOME") ? getenv("HOME") : "");
    e = getenv("NRST_CONFIG");
    if (e && *e) snprintf(config, sizeof config, "%s", e);
    else {
        snprintf(config, sizeof config, "%s", model);
        char *sl = strrchr(config, '/');
        snprintf(sl ? sl + 1 : config, sizeof config - (sl ? sl + 1 - config : 0), "config.json");
    }
    inst *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = dfn3_new(model, config, err, sizeof err);
    if (!s->m) {
        fprintf(stderr, "deep_filter_st: %s\n", err);
        free(s);
        return NULL;
    }
    apply_controls(s, 1);
    return s;
}

static void connect_port(LADSPA_Handle h, unsigned long p, LADSPA_Data *d) {
    if (p < NPORTS) ((inst *)h)->port[p] = d;
}

static void activate(LADSPA_Handle h) {
    inst *s = h;
    dfn3_reset(s->m);
    memset(s->inq, 0, sizeof s->inq);
    memset(s->outq, 0, sizeof s->outq);
    s->pos = 0;
    apply_controls(s, 1);
}

static void run(LADSPA_Handle h, unsigned long n) {
    inst *s = h;
    const LADSPA_Data *in = s->port[P_IN];
    LADSPA_Data *out = s->port[P_OUT];
    if (!in || !out) return;
    apply_controls(s, 0);
    for (unsigned long i = 0; i < n; i++) {
        float x = in[i];                     /* in and out may alias */
        out[i] = s->outq[s->pos];
        s->inq[s->pos] = x;
        if (++s->pos == DFN3_HOP) {
            dfn3_process(s->m, s->inq, s->outq);
            s->pos = 0;
        }
    }
}

static void deactivate(LADSPA_Handle h) { (void)h; }

static void cleanup(LADSPA_Handle h) {
    inst *s = h;
    dfn3_free(s->m);
    free(s);
}

static const LADSPA_Descriptor desc_mono = {
    7843797, /* not the Rust plugin's 7843795, so hosts that key on IDs can tell them apart */
    "deep_filter_mono", LADSPA_PROPERTY_HARD_RT_CAPABLE,
    "DeepFilter Mono (safetensors, C)", "nr-st (DeepFilterNet3 by Hendrik Schroeter, MIT/Apache)",
    "MIT/Apache", NPORTS, port_desc, port_name, port_hint, NULL,
    instantiate, connect_port, activate, run, NULL, NULL, deactivate, cleanup};

__attribute__((visibility("default"))) const LADSPA_Descriptor *ladspa_descriptor(unsigned long i) {
    return i == 0 ? &desc_mono : NULL;
}
