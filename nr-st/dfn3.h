/* dfn3.h: DeepFilterNet3 streaming inference from model.safetensors, one
 * 10 ms hop (480 samples at 48 kHz) per call, as libDF's DfTract::process
 * (Rikorose/DeepFilterNet v0.5.6). See dfn3.c. */
#ifndef DFN3_H
#define DFN3_H

#define DFN3_SR 48000
#define DFN3_HOP 480

typedef struct dfn3 dfn3;

/* Loads the weights (and checks config.json against the built-in
 * DeepFilterNet3 dimensions when config is not NULL and the file exists).
 * NULL on failure, with the reason in err. */
dfn3 *dfn3_new(const char *model, const char *config, char *err, int errlen);
void dfn3_free(dfn3 *d);
/* Back to the state of a fresh instance (STFT memories, norm states,
 * spectrum buffers, conv histories, GRU states). Parameters are kept. */
void dfn3_reset(dfn3 *d);
/* Same semantics as DfTract::set_atten_lim / set_pf_beta and the
 * min_db_thresh / max_db_erb_thresh / max_db_df_thresh fields. */
void dfn3_set_atten_lim(dfn3 *d, float db);
void dfn3_set_pf_beta(dfn3 *d, float beta);
void dfn3_set_thresholds(dfn3 *d, float min_db, float max_erb_db, float max_df_db);
/* One hop: in and out are DFN3_HOP samples (may alias). Returns the local
 * SNR estimate in dB. No allocation, no locks. */
float dfn3_process(dfn3 *d, const float *in, float *out);

#endif
