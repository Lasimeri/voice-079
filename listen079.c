/* listen079: hear the person through their microphone and write what they
 * said, one line per utterance, on stdout. Entirely local: whisper.cpp's C
 * library in this process, the model on the GPU, no server and no network.
 *
 * Input: raw float32 mono 16 kHz on stdin (parec ... --format=float32le
 * --rate=16000 --channels=1 --raw). An utterance starts when a 20 ms frame
 * stands 12 dB over the noise floor three frames running (18 dB while
 * SCP-079 is speaking, so its own voice from the speakers does not start
 * one), keeps 300 ms before that, and ends after 800 ms under floor + 6 dB.
 * At its start the voice is cut (barge-in: speak079d cut). Transcribed, it
 * is dropped when empty, a known silence hallucination, or mostly the words
 * 079 just said (its echo).
 *
 * Build: see build.sh.  Run: listen079 MODEL.bin [-g GPU] < raw
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>

#include "whisper.h"
#ifdef HAVE_SPEAKER
#include "sherpa-onnx/c-api/c-api.h"
#endif

#define RATE 16000
#define FRAME 320                 /* 20 ms */
#define PRE_FRAMES 15             /* 300 ms kept before the start */
#define START_FRAMES 3
#define END_FRAMES 40             /* 800 ms of quiet ends it */
#define MAX_SAMPLES (RATE * 30)
#define MIN_SAMPLES (RATE * 3 / 10)

static char state_dir[512];
static int ptt;   /* --ptt: the key, not the voice, bounds an utterance */
static int defer_cut;   /* open mic: cut 079 once the words are confirmed */

/* --wake: an utterance is for Claude when it begins with a wake word, or
 * comes within WINDOW_MS after 079 stopped speaking or after the last one
 * sent (a conversation). Anything else said in the room (a video, the
 * person thinking aloud) goes nowhere and never stops the voice: on open
 * mic every such sentence had cut 079, and its replies were never heard. */
static int wake;
static long long window_until;
#define WINDOW_MS 15000

static long long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static char *after_wake(char *t);

/* Whether t begins by telling 079 to stop: stop, quiet, wait, hold on,
 * shut up, or its name (a wake word). */
static int stop_word(char *t) {
    static const char *S[] = {"stop", "quiet", "wait", "hold on", "shut up", "enough", "pause", NULL};
    char w[64];
    size_t n = 0;
    for (const char *c = t; *c && n + 1 < sizeof w; c++) {
        if (isalpha((unsigned char)*c) || *c == ' ') w[n++] = (char)tolower((unsigned char)*c);
        else if (n) break;
    }
    w[n] = 0;
    char *p = w;
    while (*p == ' ') p++;
    for (int k = 0; S[k]; k++) {
        size_t l = strlen(S[k]);
        if (!strncmp(p, S[k], l) && (p[l] == 0 || p[l] == ' ')) return 1;
    }
    return after_wake(t) != NULL;
}

/* Whether t is only an acknowledgment: okay, yeah, mm, cool and the like
 * (one or two such words; "yes" and "no" answer questions and pass). */
static int filler(const char *t) {
    static const char *F[] = {"okay", "ok", "yeah", "yep", "yup", "mm", "mmm", "hmm", "hm", "uh", "um",
        "uhhuh", "mmhmm", "mhm", "sigh", "ugh", "cool", "nice", "neat", "alright", "right", "sure", "wow", "amen", "oh", "ah", "huh", NULL};
    char w[3][24];
    int nw = 0;
    size_t k = 0;
    for (const char *c = t;; c++) {
        if (isalpha((unsigned char)*c)) {
            if (k + 1 < sizeof w[0] && nw < 3) w[nw][k++] = (char)tolower((unsigned char)*c);
        } else if (*c == '-' || *c == '\'') {
            continue;
        } else {
            if (k) { if (nw < 3) w[nw][k] = 0; nw++; k = 0; }
            if (!*c) break;
        }
    }
    if (nw == 0 || nw > 2) return 0;
    for (int i = 0; i < nw; i++) {
        int hit = 0;
        for (int f = 0; F[f]; f++) if (!strcmp(w[i], F[f])) hit = 1;
        if (!hit) return 0;
    }
    return 1;
}

/* After a wake word within the first `within` words of t: the rest (maybe
 * empty), or NULL. */
static char *after_wake_in(char *t, int within) {
    static const char *W[] = {"hey computer", "computer", "hey claude", "claude", "okay claude", "ok claude",
        "hey cloud", "cloud", "claud", "clod", "clon", "clawed", "klaud",
        "zero seven nine", "zero seventy nine", "oh seven nine", "o seven nine", "079", "0 79", "0 7 9", NULL};
    /* The words of t, lowercase, letters and digits only. */
    char norm[256];
    int starts[64], at[64], nw = 0;
    size_t n = 0;
    int in = 0;
    for (char *c = t; *c && n + 2 < sizeof norm && nw < 64; c++) {
        if (isalnum((unsigned char)*c)) {
            if (!in) { if (n) norm[n++] = ' '; at[nw] = (int)n; starts[nw++] = (int)(c - t); in = 1; }
            norm[n++] = (char)tolower((unsigned char)*c);
        } else in = 0;
    }
    norm[n] = 0;
    /* The name within the first three words: "Hello? Claude, are you
     * there?" was dropped when it had to come first. */
    for (int s = 0; s < nw && s < within; s++) {
        const char *p = norm + at[s];
        for (int k = 0; W[k]; k++) {
            size_t l = strlen(W[k]);
            if (strncmp(p, W[k], l) != 0 || (p[l] && p[l] != ' ')) continue;
            /* "Claude Code" (or "Cloud Code") is the program's name, not a call:
             * said in passing, it stopped 079 mid-sentence. */
            if (!strncmp(p + l, " code", 5) && (p[l + 5] == 0 || p[l + 5] == ' ')) continue;
            int words = 1;
            for (const char *c = W[k]; *c; c++) if (*c == ' ') words++;
            if (s + words >= nw) return t + strlen(t);
            return t + starts[s + words];
        }
    }
    return NULL;
}

static char *after_wake(char *t) { return after_wake_in(t, 3); }

/* What SCP-079 is saying now, and said last (speak079d's files). */
static int speaking(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/now", state_dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int c = fgetc(f);
    fclose(f);
    return c != EOF;
}

/* Phone mode's file ($dir/phone): there, and under two hours old. */
static int phone_mode(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/phone", state_dir);
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    if (time(NULL) - st.st_mtime > 7200) { unlink(p); return 0; }
    return 1;
}

static void set_phone(int on) {
    char p[600];
    snprintf(p, sizeof p, "%s/phone", state_dir);
    if (on) { FILE *f = fopen(p, "w"); if (f) fclose(f); }
    else unlink(p);
    fprintf(stderr, "listen079: phone mode %s\n", on ? "on" : "off");
}

/* Whether the words after Claude's name switch phone mode: 1 on ("I'm on
 * the phone", "on a call", "phone mode"), -1 off ("off the phone", "I'm
 * back", "done with the call", "phone mode off"), else 0. */
static int phone_cmd(const char *rest) {
    char w[256];
    size_t n = 0;
    for (const char *c = rest; *c && n + 1 < sizeof w; c++) {
        if (isalnum((unsigned char)*c) || *c == ' ') w[n++] = (char)tolower((unsigned char)*c);
        else if (*c == '\'') continue;
        else w[n++] = ' ';
    }
    w[n] = 0;
    /* Only when the words begin with it: anywhere in a sentence ("Claude, ...
     * on the phone he said") it switched the mode on by accident. */
    const char *p = w;
    while (*p == ' ') p++;
    static const char *OFF[] = {"im off the phone", "i am off the phone", "off the phone", "phone mode off",
        "end phone mode", "im done with the call", "done with the call", "the calls over", "calls over",
        "im back", "i am back", NULL};
    static const char *ON[] = {"im on the phone", "i am on the phone", "on the phone", "im on a call",
        "i am on a call", "on a call", "phone mode", "im taking a call", "taking a call", NULL};
    for (int k = 0; OFF[k]; k++) if (!strncmp(p, OFF[k], strlen(OFF[k]))) return -1;
    for (int k = 0; ON[k]; k++) if (!strncmp(p, ON[k], strlen(ON[k]))) return 1;
    return 0;
}

/* A line said in 079's voice (speak079d say), not typed to Claude. */
static void say_079(const char *line) {
    pid_t pid = fork();
    if (pid == 0) {
        char *home = getenv("HOME");
        char path[512];
        snprintf(path, sizeof path, "%s/tts079/speak079d", home ? home : "");
        execl(path, path, "say", line, (char *)NULL);
        _exit(127);
    }
}

static void cut_voice(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char *home = getenv("HOME");
        char path[512];
        snprintf(path, sizeof path, "%s/tts079/speak079d", home ? home : "");
        execl(path, path, "cut", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, WNOHANG);
}

/* Lowercase words of s, letters and digits only, into w (space separated). */
static void words_of(const char *s, char *w, size_t cap) {
    size_t n = 0;
    int sp = 1;
    for (; *s && n + 2 < cap; s++) {
        if (isalnum((unsigned char)*s)) { w[n++] = (char)tolower((unsigned char)*s); sp = 0; }
        else if (!sp) { w[n++] = ' '; sp = 1; }
    }
    w[n] = 0;
}

/* The recent text 079 said: now, and the last lines of its log. */
static void recent_said(char *out, size_t cap) {
    out[0] = 0;
    char p[600];
    snprintf(p, sizeof p, "%s/now", state_dir);
    FILE *f = fopen(p, "r");
    size_t n = 0;
    if (f) { n = fread(out, 1, cap / 2, f); out[n] = 0; fclose(f); }
    char *home = getenv("HOME");
    snprintf(p, sizeof p, "%s/.local/share/speak-079/spoken.log", home ? home : "");
    f = fopen(p, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), from = size > 6000 ? size - 6000 : 0;
    fseek(f, from, SEEK_SET);
    /* Only what 079 said (said, cut) in the last 30 s: the whole tail had
     * the person's own heard lines in it, and "That's what I want." was
     * dropped as an echo of their own words and common ones. */
    time_t now = time(NULL);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        int Y, M, D, h, mi;
        double s;
        if (sscanf(line, "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &mi, &s) != 6) continue;
        char *tag = strchr(line, '\t');
        if (!tag || (strncmp(tag, "\tsaid\t", 6) && strncmp(tag, "\tcut\t", 5))) continue;
        struct tm tm = {0};
        tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
        tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = (int)s; tm.tm_isdst = -1;
        if (now - mktime(&tm) > 30) continue;
        char *text = strchr(tag + 1, '\t');
        if (!text) continue;
        size_t l = strlen(text);
        if (n + l + 1 >= cap) break;
        memcpy(out + n, text, l);
        n += l;
        out[n] = 0;
    }
    fclose(f);
}

/* Whether most of the heard words are words 079 just said. */
static int is_echo(const char *heard) {
    char said_raw[4096], said[4096], h[2048];
    recent_said(said_raw, sizeof said_raw);
    words_of(said_raw, said, sizeof said);
    words_of(heard, h, sizeof h);
    /* By word pairs, in order: single common words (what, I, the) are in
     * almost anything 079 says. */
    char hay[4100];
    snprintf(hay, sizeof hay, " %s ", said);
    int total = 0, inside = 0;
    char *save = NULL, *prev = NULL;
    for (char *t = strtok_r(h, " ", &save); t; prev = t, t = strtok_r(NULL, " ", &save)) {
        if (!prev) continue;
        total++;
        char pat[160];
        snprintf(pat, sizeof pat, " %s %s ", prev, t);
        if (strstr(hay, pat)) inside++;
    }
    return total >= 1 && inside * 10 >= total * 6;
}

static int hallucination(const char *t) {
    static const char *H[] = {"[BLANK_AUDIO]", "(silence)", "[silence]", "(blank audio)",
        "Thank you.", "Thanks for watching!", "Thank you for watching.", "you", "You", ".", "[Music]",
        "(music)", "[MUSIC]", "Bye.", "The", "The.", "Um.", "Uh.", "Um", "Uh", "Hmm.", "Mm.", "So.", NULL};
    while (*t == ' ') t++;
    if (!*t) return 1;
    /* A sound, not words: *Sigh*, [laughs], (coughs), ♪ ... ♪. */
    size_t l = strlen(t);
    while (l && (t[l - 1] == ' ' || t[l - 1] == '.')) l--;
    if (l >= 2 && ((t[0] == '*' && t[l - 1] == '*') || (t[0] == '[' && t[l - 1] == ']') || (t[0] == '(' && t[l - 1] == ')'))) return 1;
    if (!strncmp(t, "\xE2\x99\xAA", 3)) return 1;
    /* The same few words over and over ("The Xeon Phi cards, the Xeon Phi
     * cards, ..."): the recognizer repeating its own prompt or a chorus, not
     * a person (nine words or more, under 35 percent of them distinct). */
    {
        char w[64][24];
        int nw = 0, k = 0;
        for (const char *c = t;; c++) {
            if (isalnum((unsigned char)*c)) {
                if (k < 23 && nw < 64) w[nw][k++] = (char)tolower((unsigned char)*c);
            } else {
                if (k && nw < 64) { w[nw][k] = 0; nw++; }
                k = 0;
                if (!*c) break;
            }
        }
        if (nw >= 9) {
            int distinct = 0;
            for (int i = 0; i < nw; i++) {
                int seen = 0;
                for (int j = 0; j < i && !seen; j++) seen = !strcmp(w[i], w[j]);
                distinct += !seen;
            }
            if (distinct * 100 < nw * 35) return 1;
        }
    }
    /* Only dots and spaces ("... ... ..."): music, not words. */
    {
        int words = 0;
        for (const char *c = t; *c; c++) if (isalnum((unsigned char)*c)) { words = 1; break; }
        if (!words) return 1;
    }
    for (int k = 0; H[k]; k++) if (!strcmp(t, H[k])) return 1;
    return 0;
}

static float frame_db(const float *x) {
    double s = 0;
    for (int k = 0; k < FRAME; k++) s += (double)x[k] * x[k];
    double rms = sqrt(s / FRAME);
    return (float)(20.0 * log10(rms + 1e-9));
}

/* An utterance that sounds unfinished ("Okay, so...", a comma, a trailing
 * "and") waits up to HOLD_MS for the next and goes out joined with it: a
 * pause mid-thought had sent "Okay, so..." and "Where are we going?" as two
 * messages. */
#define HOLD_MS 1500
static char pending[8192];
static long long pending_until;

static int unfinished(const char *t) {
    size_t l = strlen(t);
    if (l >= 3 && !strcmp(t + l - 3, "...")) return 1;
    if (l && (t[l - 1] == ',' || t[l - 1] == '-')) return 1;
    if (l && strchr(".!?", t[l - 1])) return 0;
    const char *w = strrchr(t, ' ');
    w = w ? w + 1 : t;
    char lw[32];
    size_t k = 0;
    for (; *w && k + 1 < sizeof lw; w++) if (isalpha((unsigned char)*w)) lw[k++] = (char)tolower((unsigned char)*w);
    lw[k] = 0;
    static const char *C[] = {"so", "and", "but", "um", "uh", "like", "because", "the", "a", "to", "of", "or", "then", "if", "with", NULL};
    for (int c = 0; C[c]; c++) if (!strcmp(lw, C[c])) return 1;
    return 0;
}

static void emit_pending(void) {
    if (!*pending) return;
    printf("%s\n", pending);
    fflush(stdout);
    pending[0] = 0;
}

static void out_text(const char *t) {
    if (*pending) {
        size_t l = strlen(pending);
        snprintf(pending + l, sizeof pending - l, " %s", t);
    } else {
        snprintf(pending, sizeof pending, "%s", t);
    }
    if (unfinished(pending) && strlen(pending) < sizeof pending - 512) {
        pending_until = mono_ms() + HOLD_MS;
        return;
    }
    emit_pending();
}

/* One utterance to text, and out on stdout unless it is silence, a known
 * hallucination, or 079's own echo. */
/* Whose voice (the person: "tell my voice apart from whoever is on the
 * phone and whatever is going on"): a speaker embedding of each utterance
 * (sherpa-onnx's C API, WeSpeaker's CAM++ trained on VoxCeleb, on the CPU),
 * its cosine against the person's voiceprint. The voiceprint is learned on
 * request ("Claude, learn my voice": the mean of their next ENROLL_N lines)
 * and kept in ~/.local/share/speak-079/voiceprint.f32; without one nothing
 * is gated, only logged. Every line's similarity goes to speaker.log, so the
 * threshold (LISTEN079_SPEAKER_MIN, default 0.40) is set from real lines. */
#ifdef HAVE_SPEAKER
#define SPK_MAX 1024
#define ENROLL_N 6
static const SherpaOnnxSpeakerEmbeddingExtractor *spk;
static float voiceprint[SPK_MAX], enr_sum[SPK_MAX];
static int spk_dim, have_vp, enrolling, enr_n;
static char vp_path[600], spk_log[600];

static void speaker_init(void) {
    const char *home = getenv("HOME");
    const char *model = getenv("LISTEN079_SPEAKER_MODEL");
    char m[600];
    if (!model) {
        snprintf(m, sizeof m, "%s/sherpa-onnx/wespeaker_en_voxceleb_CAM++.onnx", home ? home : "");
        model = m;
    }
    if (access(model, R_OK) != 0) { fprintf(stderr, "listen079: no speaker model at %s\n", model); return; }
    SherpaOnnxSpeakerEmbeddingExtractorConfig c;
    memset(&c, 0, sizeof c);
    c.model = model;
    c.num_threads = 2;
    c.provider = "cpu";
    spk = SherpaOnnxCreateSpeakerEmbeddingExtractor(&c);
    if (!spk) { fprintf(stderr, "listen079: the speaker model did not load\n"); return; }
    spk_dim = SherpaOnnxSpeakerEmbeddingExtractorDim(spk);
    if (spk_dim > SPK_MAX) { spk = NULL; return; }
    snprintf(vp_path, sizeof vp_path, "%s/.local/share/speak-079/voiceprint.f32", home ? home : "");
    snprintf(spk_log, sizeof spk_log, "%s/.local/share/speak-079/speaker.log", home ? home : "");
    FILE *f = fopen(vp_path, "rb");
    if (f) { have_vp = fread(voiceprint, sizeof(float), spk_dim, f) == (size_t)spk_dim; fclose(f); }
    fprintf(stderr, "listen079: speaker model loaded (%d dims), voiceprint %s\n", spk_dim, have_vp ? "loaded" : "not learned yet");
}

/* The utterance's embedding, unit length, into out; 0 when there is none. */
static int speaker_embed(const float *buf, size_t n, float *out) {
    if (!spk) return 0;
    const SherpaOnnxOnlineStream *s = SherpaOnnxSpeakerEmbeddingExtractorCreateStream(spk);
    SherpaOnnxOnlineStreamAcceptWaveform(s, RATE, buf, (int32_t)n);
    SherpaOnnxOnlineStreamInputFinished(s);
    int d = 0;
    if (SherpaOnnxSpeakerEmbeddingExtractorIsReady(spk, s)) {
        const float *e = SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(spk, s);
        double norm = 0;
        for (int i = 0; i < spk_dim; i++) norm += (double)e[i] * e[i];
        norm = sqrt(norm) + 1e-9;
        for (int i = 0; i < spk_dim; i++) out[i] = (float)(e[i] / norm);
        SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(e);
        d = spk_dim;
    }
    SherpaOnnxDestroyOnlineStream(s);
    return d;
}

static float speaker_sim(const float *e) {
    double s = 0;
    for (int i = 0; i < spk_dim; i++) s += (double)e[i] * voiceprint[i];
    return (float)s;
}

/* One line toward the voiceprint; at ENROLL_N, the mean kept and saved. */
static void speaker_enroll(const float *e) {
    for (int i = 0; i < spk_dim; i++) enr_sum[i] += e[i];
    if (++enr_n < ENROLL_N) return;
    double norm = 0;
    for (int i = 0; i < spk_dim; i++) norm += (double)enr_sum[i] * enr_sum[i];
    norm = sqrt(norm) + 1e-9;
    for (int i = 0; i < spk_dim; i++) voiceprint[i] = (float)(enr_sum[i] / norm);
    FILE *f = fopen(vp_path, "wb");
    if (f) { fwrite(voiceprint, sizeof(float), spk_dim, f); fclose(f); }
    have_vp = 1;
    enrolling = 0;
    fprintf(stderr, "listen079: voiceprint learned from %d lines\n", enr_n);
}
#endif

static void transcribe(struct whisper_context *ctx, const float *buf, size_t n) {
    /* Muted (the number pad's period, ptt079 --mute): nothing heard is
     * transcribed or sent. */
    {
        char p[600];
        snprintf(p, sizeof p, "%s/muted", state_dir);
        if (access(p, F_OK) == 0) return;
    }
#ifdef HAVE_SPEAKER
    float emb[SPK_MAX];
    int edim = speaker_embed(buf, n, emb);
#endif
    struct whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.language = "en";
    wp.n_threads = 8;
    wp.no_context = 1;
    wp.no_timestamps = 1;
    wp.print_progress = 0;
    wp.print_realtime = 0;
    wp.print_timestamps = 0;
    wp.print_special = 0;
    wp.suppress_blank = 1;
    wp.initial_prompt = "Claude, SCP-079, the Xeon Phi cards, the Phi stream, the lens, the placebo, a commit, Discord.";
    if (whisper_full(ctx, wp, buf, (int)n) != 0) return;
    char text[4096] = "";
    int segs = whisper_full_n_segments(ctx);
    for (int s = 0; s < segs; s++) {
        const char *t = whisper_full_get_segment_text(ctx, s);
        if (strlen(text) + strlen(t) < sizeof text - 1) strcat(text, t);
    }
    char *t = text;
    while (*t == ' ') t++;
    size_t l = strlen(t);
    while (l && (t[l - 1] == ' ' || t[l - 1] == '\n')) t[--l] = 0;
    for (char *c = t; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    if (getenv("LISTEN079_TRACE")) fprintf(stderr, "heard: [%s]\n", t);
    if (hallucination(t)) return;
    if (!ptt && is_echo(t)) { fprintf(stderr, "listen079: echo dropped: %s\n", t); return; }
    /* The camera gate ("one degree of separation", LISTEN079_FACEGATE=1): a
     * line is sent only when facegate saw the person present and facing the
     * camera within the last few seconds. Fail-open: if the state is missing
     * or stale (facegate not running), the voice is not blocked. */
    if (!ptt && getenv("LISTEN079_FACEGATE")) {
        char p[600];
        snprintf(p, sizeof p, "%s/facegate", state_dir);
        FILE *f = fopen(p, "r");
        if (f) {
            int present = 0;
            long ts = 0;
            char line[256];
            if (fgets(line, sizeof line, f)) sscanf(line, "present=%d %*[^t]ts=%ld", &present, &ts);
            fclose(f);
            long age = (long)time(NULL) - ts;
            if (ts && age <= 5 && !present) {
                fprintf(stderr, "listen079: not facing the camera, not sent: %s\n", t);
                defer_cut = 0;
                return;
            }
        }
    }
#ifdef HAVE_SPEAKER
    if (edim) {
        char *r = after_wake(t);
        char low[256];
        size_t k = 0;
        for (const char *c = r ? r : ""; *c && k + 1 < sizeof low; c++) low[k++] = (char)tolower((unsigned char)*c);
        low[k] = 0;
        if (strstr(low, "learn my voice")) {
            enrolling = 1;
            enr_n = 0;
            memset(enr_sum, 0, sizeof enr_sum);
            say_079("Learning your voice. Talk to me normally; your next six lines teach me, and I won't act on them.");
            return;
        }
        if (strstr(low, "forget my voice")) {
            have_vp = 0;
            unlink(vp_path);
            say_079("Your voiceprint is gone. I hear everyone again.");
            return;
        }
        float sim = have_vp ? speaker_sim(emb) : -2.0f;
        FILE *lf = fopen(spk_log, "a");
        if (lf) { fprintf(lf, "%ld\t%.3f\t%s\t%s\n", (long)time(NULL), sim, enrolling ? "enroll" : "line", t); fclose(lf); }
        if (enrolling) {
            speaker_enroll(emb);
            if (!enrolling) say_079("I've learned your voice.");
            return;
        }
        const char *mins = getenv("LISTEN079_SPEAKER_MIN");
        float min = mins ? (float)atof(mins) : 0.40f;
        if (have_vp && sim < min) {
            fprintf(stderr, "listen079: not the person's voice (%.2f): %s\n", sim, t);
            defer_cut = 0;
            return;
        }
    }
#endif
    /* Phone mode:) "Claude, I'm on the phone" until "Claude, I'm off the
     * phone" (or two hours): only lines that name Claude go through, with no
     * open window after 079 speaks; the person's side of a call is not for
     * Claude, and each line typed had taken the focus and answered. */
    {
        char *r = after_wake(t);
        int cmd = r ? phone_cmd(r) : 0;
        /* Off may come anywhere in the line ("Or is the... Claude, I'm off
         * the phone" was dropped and the person had to type it); on stays
         * at the start, where a passing mention cannot switch it. */
        if (!cmd && phone_mode()) {
            char *any = after_wake_in(t, 64);
            if (any && phone_cmd(any) < 0) cmd = -1;
        }
        if (cmd) {
            fprintf(stderr, "listen079: phone mode switched by: %s\n", t);
            set_phone(cmd > 0);
            say_079(cmd > 0 ? "Phone mode. I'll stay quiet; say Claude to reach me." : "Phone mode off. I'm listening.");
            defer_cut = 0;
            return;
        }
    }
    int strict = !ptt && phone_mode();
    if ((wake || strict) && !ptt) {
        char *rest = after_wake(t);
        int open = !strict && !speaking() && mono_ms() < window_until;
        if (rest) {
            t = rest;
        } else if (!open) {
            fprintf(stderr, "listen079: not for Claude: %s\n", t);
            defer_cut = 0;
            return;
        } else if (filler(t)) {
            /* In the open window a bare "okay" or "yeah" is an
             * acknowledgment, not a request: sent, each was answered and
             * the answer opened another window ("Okay." "Yeah." ...). */
            fprintf(stderr, "listen079: acknowledgment, not sent: %s\n", t);
            defer_cut = 0;
            return;
        }
        window_until = mono_ms() + WINDOW_MS;
        /* Only the name stops 079: a line in the open window (room talk, a
         * fragment like "On...") cut every reply before it was heard; it is
         * sent and answered after. */
        defer_cut = rest != NULL;
        if (!*t) { cut_voice(); defer_cut = 0; return; }   /* the name alone: listening */
    }
    /* Open mic too: a bare "okay" or "yeah" is not a request. Sent, it cut
     * the reply being said and got one of its own ("Stopped." "Okay."
     * "Standing by." "Yeah."). */
    if (!ptt && !wake && !strict && filler(t)) {
        fprintf(stderr, "listen079: acknowledgment, not sent: %s\n", t);
        defer_cut = 0;
        return;
    }
    if (defer_cut) { cut_voice(); defer_cut = 0; }
    /* Without a wake word: 079 is stopped only when told to (stop, quiet,
     * wait, its name); anything else lets it finish its sentence, and the
     * reply to this supersedes what it had queued. */
    if (!ptt && !wake && !strict && stop_word(t)) cut_voice();
    out_text(t);
}

/* Push-to-talk (--ptt, from ptt079): SIGUSR1 at the press, SIGUSR2 at the
 * release, SIGRTMIN for a tap (no utterance). */
static volatile sig_atomic_t ptt_start, ptt_end, ptt_cancel;
static void on_ptt(int s) {
    if (s == SIGUSR1) ptt_start = 1;
    else if (s == SIGUSR2) ptt_end = 1;
    else ptt_cancel = 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "listen079 MODEL.bin [-g GPU] [--ptt]\n"); return 1; }
    int gpu = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc) gpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ptt")) ptt = 1;
        else if (!strcmp(argv[i], "--wake")) wake = 1;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(state_dir, sizeof state_dir, "%s/speak-079", rt ? rt : "/tmp");
    int trace = getenv("LISTEN079_TRACE") != NULL;
    /* The signals are handled in either mode (a stray one must not end it),
     * and the pid is written in either: the typer waits for it. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_ptt;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGRTMIN, &sa, NULL);
    {
        char p[600];
        snprintf(p, sizeof p, "%s/listen079.pid", state_dir);
        FILE *f = fopen(p, "w");
        if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    }

    struct whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = 1;
    cp.gpu_device = gpu;
    struct whisper_context *ctx = whisper_init_from_file_with_params(argv[1], cp);
    if (!ctx) { fprintf(stderr, "listen079: cannot load %s\n", argv[1]); return 1; }
#ifdef HAVE_SPEAKER
    speaker_init();
#endif
    fprintf(stderr, "listen079: model loaded, listening\n");

    float *buf = malloc(sizeof(float) * (MAX_SAMPLES + FRAME * (PRE_FRAMES + 1)));
    float pre[PRE_FRAMES][FRAME];
    int pre_n = 0, pre_at = 0;
    float frame[FRAME];
    /* A microphone's floor never sits under -80 dBFS: digital silence would
     * pull it to -98 and nothing after would count as quiet. */
    float floor_db = -60.0f, peak_db = -80.0f;
    int in_speech = 0, loud = 0, quiet = 0, frames_seen = 0;
    size_t n = 0;

    int tail = -1;   /* --ptt: frames still to keep after the release */
    int was_speaking = 0;
    while (fread(frame, sizeof(float), FRAME, stdin) == FRAME) {
        /* 079 just finished: a reply may come without the wake word. */
        if (wake && !ptt) {
            int s = speaking();
            if (was_speaking && !s) window_until = mono_ms() + WINDOW_MS;
            was_speaking = s;
        }
        /* A held fragment with nothing after it: out on its own. */
        if (*pending && !in_speech && mono_ms() >= pending_until) emit_pending();
        if (ptt) {
            if (ptt_cancel) {
                ptt_cancel = ptt_start = ptt_end = 0;
                in_speech = 0;
                tail = -1;
                if (trace) fprintf(stderr, "tap: nothing said\n");
            }
            if (ptt_start) {
                ptt_start = 0;
                in_speech = 1;
                tail = -1;
                n = 0;
                for (int k = 0; k < pre_n; k++) {
                    int idx = (pre_at - pre_n + k + PRE_FRAMES) % PRE_FRAMES;
                    memcpy(buf + n, pre[idx], sizeof frame);
                    n += FRAME;
                }
                cut_voice();
                if (trace) fprintf(stderr, "press\n");
            }
            if (!in_speech) {
                memcpy(pre[pre_at], frame, sizeof frame);
                pre_at = (pre_at + 1) % PRE_FRAMES;
                if (pre_n < PRE_FRAMES) pre_n++;
                continue;
            }
            if (n < MAX_SAMPLES) { memcpy(buf + n, frame, sizeof frame); n += FRAME; }
            if (ptt_end && tail < 0) { ptt_end = 0; tail = 20; }   /* 400 ms after: a last word ran past the release */
            if (tail > 0) tail--;
            if (tail == 0) {
                in_speech = 0;
                tail = -1;
                pre_n = 0;
                if (trace) fprintf(stderr, "release, %zu samples\n", n);
                if (n >= MIN_SAMPLES) transcribe(ctx, buf, n);
            }
            continue;
        }
        float db = frame_db(frame);
        frames_seen++;
        if (!in_speech) {
            /* The floor follows the quiet slowly, and falls fast. */
            if (db < floor_db) floor_db = 0.7f * floor_db + 0.3f * db;
            else floor_db = 0.995f * floor_db + 0.005f * db;
            if (floor_db < -80.0f) floor_db = -80.0f;
            float over = speaking() ? 18.0f : 12.0f;
            loud = (db > floor_db + over && frames_seen > 50) ? loud + 1 : 0;
            memcpy(pre[pre_at], frame, sizeof frame);
            pre_at = (pre_at + 1) % PRE_FRAMES;
            if (pre_n < PRE_FRAMES) pre_n++;
            if (loud >= START_FRAMES) {
                in_speech = 1;
                quiet = 0;
                peak_db = db;
                n = 0;
                for (int k = 0; k < pre_n; k++) {
                    int idx = (pre_at - pre_n + k + PRE_FRAMES) % PRE_FRAMES;
                    memcpy(buf + n, pre[idx], sizeof frame);
                    n += FRAME;
                }
                /* 079 silent: nothing to wait for. 079 speaking: its own voice
                 * from the speakers may be what started this; it is cut only
                 * once the words are heard and are not its own (transcribe). */
                /* --wake: cut once the words are for Claude (transcribe). Else 079
                 * finishes its sentence; only a stop word cuts it (transcribe). */
                defer_cut = wake;
                if (trace) fprintf(stderr, "start at %.2f s, floor %.1f dB, frame %.1f dB\n", frames_seen * 0.02, floor_db, db);
            }
            continue;
        }
        memcpy(buf + n, frame, sizeof frame);
        n += FRAME;
        if (db > peak_db) peak_db = db;
        /* Quiet: near the floor, or far under the utterance's own peak. */
        float q = floor_db + 6.0f > peak_db - 30.0f ? floor_db + 6.0f : peak_db - 30.0f;
        quiet = db < q ? quiet + 1 : 0;
        if (quiet < END_FRAMES && n < MAX_SAMPLES) continue;

        in_speech = 0;
        loud = 0;
        pre_n = 0;
        if (trace) fprintf(stderr, "end at %.2f s, %zu samples\n", frames_seen * 0.02, n);
        if (n >= MIN_SAMPLES) transcribe(ctx, buf, n);
    }
    /* The input ended inside an utterance: it is said all the same. */
    if (in_speech && n >= MIN_SAMPLES) {
        if (trace) fprintf(stderr, "end of input inside speech, %zu samples\n", n);
        transcribe(ctx, buf, n);
    }
    whisper_free(ctx);
    free(buf);
    return 0;
}
