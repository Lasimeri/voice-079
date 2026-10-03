/* tts079: speech in the voice of SCP-079 (SBTalker, the Sound Blaster
 * text-to-speech engine of Dr. Sbaitso), from text, as an 8-bit WAV at
 * 8522 Hz. A C port of Eibriel's godot-tts-079 (MIT code, CC-BY-4.0 data,
 * https://codeberg.org/Eibriel/godot-tts-079), itself reverse-engineered
 * from SBTALKER.EXE and checked against DOSBox captures.
 *
 * Where the GDScript slips from what its own comments say the engine does,
 * this follows the comments (the engine), and --godot gives the GDScript's
 * behaviour for comparison:
 *   - a reversed chain plays a reversed copy (Godot's Array.reverse() turned
 *     the shared table itself around), and only when the chain is purely
 *     voiced (the code reversed when any block was);
 *   - suffix tests compare the slice word[pos:pos+len] (Godot's substr takes
 *     a length, so substr(pos, end) ran past the suffix);
 *   - a right context running past the word fails (a missing return).
 *
 * Build: tcc -O2 -o tts079 tts079.c   (or gcc -O2)
 * Run:   tts079 [-p PITCH] [--godot] [--g2p] [-o FILE.wav] [TEXT...]
 *        no TEXT: read stdin; no -o: the WAV goes to stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "tts079_data.h"

#define RATE 8522
#define NPHON 44

static int godot_mode = 0;

/* ── growable byte and int buffers ───────────────────────────────────── */

typedef struct { uint8_t *p; size_t n, cap; } Bytes;
typedef struct { int *p; size_t n, cap; } Ints;

static void bput(Bytes *b, uint8_t v) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 4096; b->p = realloc(b->p, b->cap); }
    b->p[b->n++] = v;
}
static void iput(Ints *a, int v) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 256; a->p = realloc(a->p, a->cap * sizeof(int)); }
    a->p[a->n++] = v;
}

typedef struct { char *p; size_t n, cap; } Str;
static void sputc(Str *s, char c) {
    if (s->n + 1 >= s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->p = realloc(s->p, s->cap); }
    s->p[s->n++] = c; s->p[s->n] = 0;
}
static void sputs(Str *s, const char *t) { while (*t) sputc(s, *t++); }

/* ── data, indexed ──────────────────────────────────────────────────── */

#define NRULES (sizeof RULES / sizeof RULES[0])
#define NTRANS (sizeof TRANS / sizeof TRANS[0])
#define NSEGS (sizeof SEGS / sizeof SEGS[0])

static const Trans *trans_at[NPHON][NPHON];
static const uint8_t *block_ptr[4096];
static int block_len[4096];
static int nblocks;

/* Segment lists by pointer: the first entry and the count (sorted by ptr). */
typedef struct { int ptr, first, count; } List;
static List lists[1024];
static int nlists;
/* The entries as played; --godot reverses them in place, as Godot did. */
static Seg segs[NSEGS];

static void load(void) {
    for (size_t i = 0; i < NTRANS; i++)
        if (TRANS[i].l < NPHON && TRANS[i].r < NPHON) trans_at[TRANS[i].l][TRANS[i].r] = &TRANS[i];
    size_t pos = 0;
    while (pos < sizeof BLOCKS_BIN && nblocks < 4096) {
        int ln = BLOCKS_BIN[pos];
        block_ptr[nblocks] = BLOCKS_BIN + pos + 1;
        block_len[nblocks] = ln;
        if (pos + 1 + ln > sizeof BLOCKS_BIN) block_len[nblocks] = (int)(sizeof BLOCKS_BIN - pos - 1);
        nblocks++;
        pos += 1 + ln;
    }
    memcpy(segs, SEGS, sizeof SEGS);
    for (size_t i = 0; i < NSEGS; i++) {
        if (nlists && lists[nlists - 1].ptr == segs[i].ptr) { lists[nlists - 1].count++; continue; }
        lists[nlists].ptr = segs[i].ptr; lists[nlists].first = (int)i; lists[nlists].count = 1; nlists++;
    }
}

static List *list_of(int ptr) {
    for (int i = 0; i < nlists; i++) if (lists[i].ptr == ptr) return &lists[i];
    return NULL;
}

/* ── G2P (g2p_engine.gd) ────────────────────────────────────────────── */

/* A word's character, or the sentinel '\0' past either end ('x00'). */
static char at(const char *w, int n, int i) { return (i >= 0 && i < n) ? w[i] : '\0'; }
static int is_alpha(char c) { return c >= 'A' && c <= 'Z'; }
static int is_vowel(char c) { return (c && strchr("AEIOUY", c)) || c == '\0'; }
static int is_consonant(char c) { return is_alpha(c) && !strchr("AEIOUY", c); }
static int in_set(char c, const char *set) { return c && strchr(set, c) != NULL; }

/* The right context: literal letters, apostrophes and periods after pos. */
static int match_rctx(const char *w, int n, int pos, const char *pat, int *end) {
    int i = pos;
    for (const char *p = pat; *p; p++) {
        if (i >= n) return 0;
        char ch = *p;
        if (ch == '\'' || ch == '.' || is_alpha(ch)) {
            if (w[i] != ch) return 0;
        } else return 0;
        i++;
    }
    *end = i;
    return 1;
}

static int slice_is(const char *w, int n, int pos, const char *s) {
    int l = (int)strlen(s);
    return pos >= 0 && pos + l <= n && strncmp(w + pos, s, l) == 0;
}

static int suffix_percent(const char *w, int n, int pos) {
    static const char *sufs[] = {"ERS", "ELY", "ING", "ER", "ED", "ES", "E"};
    for (int k = 0; k < 7; k++) {
        int end = pos + (int)strlen(sufs[k]);
        int hit = godot_mode
            /* Godot: substr(pos, end) is end characters from pos. */
            ? (pos >= 0 && pos <= n && (int)strlen(w + pos) >= 0 &&
               strncmp(w + pos, sufs[k], strlen(sufs[k])) == 0 &&
               (n - pos <= end ? (n - pos) == (int)strlen(sufs[k]) : end == (int)strlen(sufs[k])))
            : slice_is(w, n, pos, sufs[k]);
        if (hit && (end >= n || w[end] == ' ')) return 1;
    }
    return 0;
}

/* Godot's substr(pos, pos+4) == s: pos+4 characters from pos. */
static int godot_substr_is(const char *w, int n, int pos, const char *s) {
    int len = pos + (int)strlen(s);
    int avail = n - pos;
    if (pos < 0 || avail < 0) return 0;
    int got = avail < len ? avail : len;
    return got == (int)strlen(s) && strncmp(w + pos, s, got) == 0;
}

static int suffix_bang(const char *w, int n, int pos) {
    if (pos >= n || !is_alpha(w[pos])) return 1;
    char ch = w[pos], nx = at(w, n, pos + 1);
    if (ch == 'S' && !is_alpha(nx)) return 1;
    if (ch == 'L' && nx == 'Y' && (pos + 2 >= n || !is_alpha(w[pos + 2]))) return 1;
    int ment = godot_mode ? godot_substr_is(w, n, pos, "MENT") : slice_is(w, n, pos, "MENT");
    if (ment && (pos + 4 >= n || !is_alpha(w[pos + 4]))) return 1;
    int ness = godot_mode ? godot_substr_is(w, n, pos, "NESS") : slice_is(w, n, pos, "NESS");
    if (ness && (pos + 4 >= n || !is_alpha(w[pos + 4]))) return 1;
    return 0;
}

static int suffix_dash(const char *w, int n, int pos) {
    char ch = at(w, n, pos), nx = at(w, n, pos + 1);
    if (ch == 'Y' && !is_alpha(nx)) return 1;
    if (ch == 'I' && nx == 'E' && (pos + 2 >= n || !is_alpha(w[pos + 2]))) return 1;
    return 0;
}

static int match_bctx(const char *w, int n, int pos, const char *pat) {
    int i = pos;
    for (const char *p = pat; *p; p++) {
        char ch = *p, wch = at(w, n, i);
        if (ch == '#') {
            if (!is_vowel(wch)) return 0;
            i++;
            while (i < n && is_vowel(w[i])) i++;
        } else if (ch == '^') {
            if (wch == 'Q' && i + 1 < n && w[i + 1] == 'U') i += 2;
            else if (is_consonant(wch)) i++;
            else return 0;
        } else if (ch == '*') {
            int qu = wch == 'Q' && i + 1 < n && w[i + 1] == 'U';
            if (!is_consonant(wch) && !qu) return 0;
            i += qu ? 2 : 1;
            while (i < n && is_consonant(w[i])) i++;
        } else if (ch == ':') {
            while (i < n && is_consonant(w[i])) i++;
        } else if (ch == '+') {
            if (!in_set(wch, "EIY")) return 0;
            i++;
        } else if (ch == '.') {
            if (!in_set(wch, "BDJGLMNRVWZ")) return 0;
            i++;
        } else if (ch == '%') {
            if (!suffix_percent(w, n, i)) return 0;
        } else if (ch == '!') {
            if (!suffix_bang(w, n, i)) return 0;
        } else if (ch == '-') {
            if (!suffix_dash(w, n, i)) return 0;
        } else if (ch == ' ' || ch == '\'') {
            if (wch != ch) return 0;
            i++;
        } else if (is_alpha(ch)) {
            if (wch != ch) return 0;
            i++;
        } else return 0;
    }
    return 1;
}

static int match_lctx(const char *w, int n, int pos, const char *pat) {
    int i = pos - 1;
    for (const char *p = pat; *p; p++) {
        char ch = *p, wch = at(w, n, i);
        if (ch == '#') {
            if (!is_vowel(wch)) return 0;
            i--;
            while (i >= 0 && is_vowel(w[i])) i--;
        } else if (ch == '^') {
            if (!is_consonant(wch)) return 0;
            i--;
        } else if (ch == '*') {
            if (!is_consonant(wch)) {
                if (wch == 'U' && i - 1 >= 0 && w[i - 1] == 'Q') i -= 2;
                else return 0;
            } else {
                i--;
                while (i >= 0 && is_consonant(w[i])) i--;
            }
        } else if (ch == ':') {
            while (i >= 0 && is_consonant(w[i])) i--;
        } else if (ch == '+') {
            if (!in_set(wch, "EIY")) return 0;
            i--;
        } else if (ch == '.') {
            if (!in_set(wch, "BDJGLMNRVWZ")) return 0;
            i--;
        } else if (ch == '@') {
            if (wch == 'H') {
                if (!in_set(at(w, n, i - 1), "CST")) return 0;
                i -= 2;
            } else if (in_set(wch, "DJLNRSTWZ")) i--;
            else return 0;
        } else if (ch == '&') {
            if (wch == 'H' && i - 1 >= 0 && in_set(w[i - 1], "CS")) i -= 2;
            else if (in_set(wch, "SZ")) i--;
            else return 0;
        } else if (ch == ' ' || ch == '\'') {
            if (wch != ch) return 0;
            i--;
        } else if (is_alpha(ch)) {
            if (wch != ch) return 0;
            i--;
        } else return 0;
    }
    return 1;
}

/* Uppercase, no periods, only A-Z ' , and spaces; words that hold a
 * letter or a comma, joined by single spaces between a leading and a
 * trailing space. */
static void preprocess(const char *s, Str *full) {
    Str clean = {0};
    for (const char *p = s; *p; p++) {
        char c = (char)toupper((unsigned char)*p);
        if (c == '.') continue;
        if (is_alpha(c) || c == '\'' || c == ',' || c == ' ') sputc(&clean, c);
    }
    sputc(full, ' ');
    int first = 1;
    const char *p = clean.p ? clean.p : "";
    while (*p) {
        while (*p == ' ') p++;
        const char *a = p;
        while (*p && *p != ' ') p++;
        if (p == a) break;
        int keep = 0;
        for (const char *q = a; q < p; q++) if (is_alpha(*q) || *q == ',') keep = 1;
        if (!keep) continue;
        if (!first) sputc(full, ' ');
        for (const char *q = a; q < p; q++) sputc(full, *q);
        first = 0;
    }
    sputc(full, ' ');
    free(clean.p);
}

static void g2p_sentence(const char *sentence, Str *out) {
    Str full = {0};
    preprocess(sentence, &full);
    const char *w = full.p;
    int n = (int)full.n;
    int i = 1;
    while (i < n) {
        char ch = w[i];
        if (ch == ' ' || ch == '\'' || ch == '.') { i++; continue; }
        if (ch == ',') { sputs(out, "  "); i++; continue; }
        if (!is_alpha(ch)) { i++; continue; }
        const char *phon = "";
        int adv = 1;
        for (size_t r = 0; r < NRULES; r++) {
            if (RULES[r].letter != ch) continue;
            int end;
            if (!match_rctx(w, n, i + 1, RULES[r].rctx, &end)) continue;
            if (!match_lctx(w, n, i, RULES[r].lctx)) continue;
            if (!match_bctx(w, n, end, RULES[r].bctx)) continue;
            phon = RULES[r].phon;
            adv = end - i;
            break;
        }
        sputs(out, phon);
        i += adv;
    }
    /* Terminal punctuation of the sentence as given, for the prosody. */
    int l = (int)strlen(sentence);
    while (l > 0 && isspace((unsigned char)sentence[l - 1])) l--;
    if (l > 0 && strchr("!.;?", sentence[l - 1])) sputc(out, sentence[l - 1]);
    free(full.p);
}

/* ── digit prosody (prosody.gd) ─────────────────────────────────────── */

static int vowel_start(char c) { return c && strchr("AEIOU", c); }

static void insert_digit_prosody(const char *s, Str *out) {
    int len = (int)strlen(s), N = 0;
    char term = 0;
    for (int i = 1; i < len; i++) {
        if (vowel_start(s[i])) { N++; i++; }
        if (i < len && strchr("!.;?", s[i]) && s[i]) { term = s[i]; break; }
    }
    if (N > 100) N = 100;
    if (N < 1) { sputs(out, s); return; }
    int *a = calloc((size_t)N + 1, sizeof(int));
    for (int i = 1; i <= N; i++) a[i] = 7 - ((N / 2 + 4 * i) / N);
    if (term == '.') {
        if (N >= 1) a[N] = 0;
        if (N >= 2) a[N - 1] = 1;
        if (N >= 3) a[N - 2] = 2;
    } else if (term == ';') {
        a[N] = 2;
    } else if (term == '?') {
        a[N] = 9;
        if (N >= 2) a[N - 1] = 7;
    } else if (term == '!') {
        for (int i = 1; i <= N; i++) if (a[i] < 8) a[i] += 2;
        a[N] = 9;
        if (N >= 2) a[N - 1] = 8;
        a[1] = 9;
    }
    if (len) sputc(out, s[0]);
    int di = 0, last = 5, delta = 0, stress = 0;
    for (int i = 1; i < len;) {
        char c = s[i];
        if (c == '#') { stress++; i++; continue; }
        if (c == '@') { if (stress > 0) stress--; i++; continue; }
        if (c == '/') { delta++; i++; continue; }
        if (c == '\\') { delta--; i++; continue; }
        if (vowel_start(c)) {
            di++;
            if (di > 1 && di <= N && stress == 0) {
                int v = a[di] + delta;
                if (v < 0) v = 0;
                if (v > 9) v = 9;
                if (v != last) { sputc(out, (char)('0' + v)); last = v; }
            }
            sputc(out, c);
            i++;
            if (i < len) sputc(out, s[i]);
            i++;
        } else {
            sputc(out, c);
            i++;
        }
    }
    free(a);
}

/* ── converter: display string to synthesis bytes (tts.gd) ──────────── */

static int digraph(char a, char b) {
    static const struct { const char *d; int v; } D[] = {
        {"AA", 7}, {"AH", 5}, {"AX", 6}, {"AY", 37}, {"AE", 4}, {"EH", 3}, {"AW", 42},
        {"EY", 38}, {"AO", 10}, {"DH", 26}, {"DX", 15}, {"OY", 41}, {"ZH", 28}, {"UH", 8},
        {"OW", 9}, {"IH", 1}, {"IX", 2}, {"IY", 39}, {"UW", 40}, {"TH", 22}, {"TX", 14},
        {"SH", 24}, {"KX", 17}, {"PX", 12}, {"ER", 43}, {"NG", 32}};
    for (size_t k = 0; k < sizeof D / sizeof D[0]; k++)
        if (D[k].d[0] == a && D[k].d[1] == b) return D[k].v;
    return -1;
}

static int lowercase_cons(char c) {
    switch (c) {
    case 'b': return 18; case 'd': return 19; case 'f': return 21; case 'g': return 20;
    case 'h': return 34; case 'k': return 16; case 'l': return 29; case 'm': return 30;
    case 'n': return 31; case 'p': return 11; case 'r': return 35; case 's': return 23;
    case 't': return 13; case 'v': return 25; case 'w': return 36; case 'y': return 33;
    case 'z': return 27;
    }
    return -1;
}

static void convert(const char *d, Ints *out) {
    int len = (int)strlen(d), i = 0;
    if (i < len && d[i] == ' ') i++;
    while (i < len) {
        char c = d[i];
        if (strchr("!.;?", c)) break;
        if (isspace((unsigned char)c)) { iput(out, 0); i++; continue; }
        if (c >= '0' && c <= '9') { iput(out, 55 - (c - '0')); i++; continue; }
        if (c == '[') { iput(out, 44); i++; continue; }
        if (c == ']') { iput(out, 45); i++; continue; }
        if (c == '\\') { iput(out, 57); i++; continue; }
        if (c == '/') { iput(out, 56); i++; continue; }
        int lc = lowercase_cons(c);
        if (lc >= 0) { iput(out, lc); i++; continue; }
        if (is_alpha(c) && i + 1 < len) {
            int dg = digraph(c, d[i + 1]);
            if (dg >= 0) { iput(out, dg); i += 2; continue; }
        }
        i++;
    }
    iput(out, 0);
}

/* ── synthesis (tts.gd) ─────────────────────────────────────────────── */

typedef struct { int idx, b8a, a7a; } Phon;

static const int SPEED_TABLE[] = {32, 36, 43, 56, 96, 10000, 128, 64, 42, 32};

static int parse_synth(const int *raw, int n, int pitch, Phon *out) {
    int b8a = 0, a7c = 0, m = 0;
    for (int i = 0; i < n;) {
        int b = raw[i];
        if (b < 44) {
            out[m].idx = b; out[m].b8a = b8a; out[m].a7a = a7c; m++;
            a7c = 0;
            i++;
        } else if (b == 44 || b == 45) {
            int nx = i + 1 < n ? raw[i + 1] : 0;
            if (nx >= 46 && nx <= 55) { a7c = b == 44 ? 55 - nx : nx - 55; i += 2; }
            else { a7c = b == 44 ? 5 : -5; i++; }
        } else if (b >= 46 && b <= 55) {
            b8a = (b - 45 - pitch) * 10;
            int nx = i + 1 < n ? raw[i + 1] : 0;
            if (nx >= 46 && nx <= 55) { b8a += nx - 55; i += 2; }
            else i++;
        } else if (b == 57) {
            b8a = b8a + 10 > 70 ? 70 : b8a + 10;
            i++;
        } else if (b == 56) {
            b8a = b8a - 10 < -50 ? -50 : b8a - 10;
            i++;
        } else i++;
    }
    return m;
}

static int speed_idx(int b8a, int pitch, int a7a) {
    int v = pitch + a7a + (2 * b8a) / 19;   /* C division truncates, as x86 IDIV */
    return v < 0 ? 0 : v > 9 ? 9 : v;
}

static void emit_block(Bytes *out, int block, int count) {
    if (block < 0 || block >= nblocks) return;
    int n = count < block_len[block] ? count : block_len[block];
    for (int k = 0; k < n; k++) bput(out, block_ptr[block][k]);
}

/* One chain of blocks (play_chain_blocks); returns b88 and a80. */
static void play_chain(int ptr, int t_rev, Bytes *out, int *b88p, int b89, int *a80p, int si) {
    List *L = list_of(ptr);
    if (!L || L->count == 0) return;
    Seg *e = segs + L->first;
    int cnt = L->count;
    int any_voiced_path = 0, all_voiced_path = 1;
    for (int k = 0; k < cnt; k++) {
        if (!e[k].is_voiced) any_voiced_path = 1;
        else all_voiced_path = 0;
    }
    /* The order: reversed only for a purely voiced-path chain (its blocks
     * all is_voiced=0); --godot: when any is, and the table itself. */
    int reverse = t_rev && (godot_mode ? any_voiced_path : all_voiced_path);
    Seg tmp[256];
    Seg *order = e;
    if (reverse) {
        if (godot_mode) {
            for (int a = 0, b = cnt - 1; a < b; a++, b--) { Seg t = e[a]; e[a] = e[b]; e[b] = t; }
        } else if (cnt <= 256) {
            for (int k = 0; k < cnt; k++) tmp[k] = e[cnt - 1 - k];
            order = tmp;
        }
    }
    int b88 = *b88p, a80 = *a80p, di = si * 2;
    for (int k = 0; k < cnt; k++) {
        Seg *s = &order[k];
        int period_len = s->period_len, a82 = 1;
        if (!s->is_voiced) {
            /* Voiced path (CS:0x07CA). */
            while (a82 > 0) {
                int diff = b89 - b88, step = 0;
                if (diff > 0) step = (diff >> 4) + 1;
                else if (diff < 0) step = diff >> 4;   /* arithmetic, as SAR */
                b88 += step;
                int b86;
                if (b88 > 0) {
                    /* The DAC holds its last value through the period extension. */
                    uint8_t last = out->n ? out->p[out->n - 1] : 0;
                    for (int z = 0; z < b88; z++) bput(out, last);
                    b86 = period_len;
                } else {
                    b86 = period_len >= 75 ? period_len + b88 : period_len;
                    if (b86 < 0) b86 = 0;
                }
                a80 -= 16;
                if (a80 <= 0) {
                    a80 += SPEED_TABLE[si];
                    if (di > 10) { if (--a82 == 0) break; }
                    else if (di < 10) a82++;
                }
                if (b86 > 0) emit_block(out, s->block_idx, b86);
                a82--;
            }
        } else {
            /* Unvoiced path (CS:0x07CD). */
            int b87 = period_len >> 3, b86 = period_len + b87;
            while (a82 > 0) {
                b86 -= b87;
                if (b86 < b87) b86 = b87;
                if (t_rev) {
                    a80 -= 16;
                    if (a80 <= 0) {
                        a80 += SPEED_TABLE[si];
                        if (di > 10) { if (--a82 == 0) break; }
                        else if (di < 10) a82++;
                    }
                }
                if (b86 > 0) emit_block(out, s->block_idx, b86);
                a82--;
            }
        }
    }
    *b88p = b88;
    *a80p = a80;
}

static void synthesise(const char *text, int pitch, Bytes *out, Str *phonemes) {
    /* SBTalker appends a period when the input ends without terminal punctuation. */
    Str norm = {0};
    sputs(&norm, text);
    while (norm.n && isspace((unsigned char)norm.p[norm.n - 1])) norm.p[--norm.n] = 0;
    if (norm.n && !strchr("!.;?", norm.p[norm.n - 1])) sputc(&norm, '.');
    if (!norm.n) { free(norm.p); return; }

    Str g2p = {0}, spaced = {0}, display = {0};
    g2p_sentence(norm.p, &g2p);
    if (phonemes) sputs(phonemes, g2p.p ? g2p.p : "");
    sputc(&spaced, ' ');
    sputs(&spaced, g2p.p ? g2p.p : "");
    insert_digit_prosody(spaced.p, &display);

    Ints raw = {0};
    iput(&raw, 0);                        /* the leading SIL */
    convert(display.p ? display.p : "", &raw);
    for (int k = 0; k < 10; k++) iput(&raw, 0);   /* the engine's trailing SILs */

    Phon *seq = malloc(sizeof(Phon) * (raw.n + 1));
    int m = parse_synth(raw.p, (int)raw.n, pitch, seq);
    int last = m - 2, b88 = 0, a80 = SPEED_TABLE[5];
    for (int i = 0; i < m - 1; i++) {
        Phon p = seq[i], c = seq[i + 1];
        if (p.idx >= NPHON || c.idx >= NPHON) continue;
        const Trans *t = trans_at[p.idx][c.idx];
        if (!t) continue;
        if (i > 0 && t->t1 >= 0)
            play_chain(t->t1, t->t1_rev, out, &b88, p.b8a, &a80, speed_idx(p.b8a, pitch, p.a7a));
        if (i < last && t->t2 >= 0) {
            int si = speed_idx(c.b8a, pitch, c.a7a);
            a80 = SPEED_TABLE[si];
            play_chain(t->t2, t->t2_rev, out, &b88, c.b8a, &a80, si);
        }
    }
    free(seq); free(raw.p); free(norm.p); free(g2p.p); free(spaced.p); free(display.p);
}

/* ── a response's markdown as sentences to say (--md) ───────────────── */

static const char *ONES[] = {"zero", "one", "two", "three", "four", "five", "six", "seven",
    "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
    "seventeen", "eighteen", "nineteen"};
static const char *TENS[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
    "eighty", "ninety"};
static const char *LETTERS[] = {"ay", "bee", "see", "dee", "ee", "eff", "jee", "aitch", "eye",
    "jay", "kay", "ell", "em", "en", "oh", "pee", "queue", "ar", "ess", "tee", "you", "vee",
    "double you", "ex", "why", "zee"};

static void say_below_1000(Str *o, int v) {
    if (v >= 100) {
        sputs(o, ONES[v / 100]); sputs(o, " hundred");
        v %= 100;
        if (v) sputc(o, ' ');
    }
    if (v >= 20) {
        sputs(o, TENS[v / 10]);
        if (v % 10) { sputc(o, ' '); sputs(o, ONES[v % 10]); }
    } else if (v > 0) sputs(o, ONES[v]);
}

static void say_number(Str *o, const char *d, int len) {
    /* Long runs (hashes, ids): digit by digit. */
    if (len > 9 || (len > 1 && d[0] == '0')) {
        for (int k = 0; k < len; k++) { if (k) sputc(o, ' '); sputs(o, ONES[d[k] - '0']); }
        return;
    }
    long v = 0;
    for (int k = 0; k < len; k++) v = v * 10 + (d[k] - '0');
    if (v == 0) { sputs(o, "zero"); return; }
    /* Four digits as two pairs, the way years and model numbers are said:
     * 3120 "thirty one twenty", 2026 "twenty twenty six", 1080 "ten
     * eighty", 1500 "fifteen hundred"; round thousands stay "two thousand"
     * ("three thousand one hundred twenty" for a card's name was a mouthful). */
    if (len == 4 && v % 1000 != 0) {
        int hi = (int)(v / 100), lo = (int)(v % 100);
        say_below_1000(o, hi);
        if (lo == 0) sputs(o, " hundred");
        else if (lo < 10) { sputs(o, " oh "); sputs(o, ONES[lo]); }
        else { sputc(o, ' '); say_below_1000(o, lo); }
        return;
    }
    static const char *SCALE[] = {"", " thousand", " million"};
    int parts[3] = {(int)(v % 1000), (int)(v / 1000 % 1000), (int)(v / 1000000)};
    int first = 1;
    for (int s = 2; s >= 0; s--) {
        if (!parts[s]) continue;
        if (!first) sputc(o, ' ');
        say_below_1000(o, parts[s]);
        sputs(o, SCALE[s]);
        first = 0;
    }
}

/* The acronyms spelled out when written in capitals; any other capitals
 * word of three or more letters with a vowel is said as a word ("THE MOON"
 * on a poster was spelled "tee aitch ee em oh oh en"). */
static const char *ACRONYMS[] = {"GPU", "CPU", "USB", "API", "AEC", "SSH", "VLC", "MCP", "KDE", "LLM", "TUI",
    "DAC", "CRT", "OS", "UI", "CLI", "RAM", "ROM", "SSD", "HDD", "LED", "LAN", "IP", "DNS", "URL", "HTML",
    "CSS", "TCP", "UDP", "SQL", "PDF", "AVX", "KNC", "VPU", "NAS", "ESP", "SDR", "TTS", "STT", "VAD", "MIT",
    "IDE", "PR", "UPS", "AI", "PC", "TV", "OK", "ID", "FPS", "RGB", "NPU", "MPSS", "MOE", "KV", "BF", "ARM", "HDMI", "DVI", "VGA", "LCD", "PSU", "AUR", "DDC", "DP", "HD", "SD", "DMA", "SSE", "AVX", "MCU", "NSA", "CIA", "FBI", "USA", "UK", "EU", "VHS", "DVD", "CD", "MP", "TTY", "IRL", "AFK", NULL};

static int is_acronym(const char *w, int len) {
    for (int k = 0; ACRONYMS[k]; k++)
        if ((int)strlen(ACRONYMS[k]) == len && !strncmp(ACRONYMS[k], w, len)) return 1;
    int vowel = 0;
    for (int k = 0; k < len; k++) if (strchr("AEIOUY", w[k])) vowel = 1;
    return len <= 2 || !vowel;
}

/* A word as spoken: an acronym (2 to 5 capitals) spelled out, letters
 * and digits apart, digits as numbers. */
static void say_word(Str *o, const char *w, int len) {
    int caps = len >= 2 && len <= 5, alpha = 0;
    for (int k = 0; k < len; k++) {
        if (isalpha((unsigned char)w[k])) alpha++;
        if (!isupper((unsigned char)w[k])) caps = 0;
    }
    if (caps && !is_acronym(w, len)) {
        for (int k = 0; k < len; k++) sputc(o, (char)tolower((unsigned char)w[k]));
        return;
    }
    if (caps) {
        for (int k = 0; k < len; k++) { if (k) sputc(o, ' '); sputs(o, LETTERS[w[k] - 'A']); }
        return;
    }
    int k = 0;
    while (k < len) {
        int a = k;
        if (isdigit((unsigned char)w[k])) {
            while (k < len && isdigit((unsigned char)w[k])) k++;
            if (a && o->n && o->p[o->n - 1] != ' ') sputc(o, ' ');
            say_number(o, w + a, k - a);
            /* A decimal point and its digits. */
            if (k + 1 < len && w[k] == '.' && isdigit((unsigned char)w[k + 1])) {
                sputs(o, " point");
                k++;
                while (k < len && isdigit((unsigned char)w[k])) { sputc(o, ' '); sputs(o, ONES[w[k] - '0']); k++; }
            }
            if (k < len) sputc(o, ' ');
        } else {
            while (k < len && !isdigit((unsigned char)w[k])) k++;
            for (int j = a; j < k; j++) sputc(o, w[j]);
        }
    }
    (void)alpha;
}

/* One line of prose, its inline markdown and symbols as words, into o. */
static void say_line(Str *o, const char *s) {
    Str t = {0};
    for (const char *p = s; *p;) {
        unsigned char c = (unsigned char)*p;
        if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) {
            while (*p && !isspace((unsigned char)*p) && *p != ')') p++;
            sputs(&t, " link ");
            continue;
        }
        if (c == '[') { p++; continue; }                     /* [text](url): the text */
        if (c == ']' && p[1] == '(') {
            p += 2;
            while (*p && *p != ')') p++;
            if (*p) p++;
            continue;
        }
        if (!strncmp(p, "\xE2\x86\x92", 3)) { sputs(&t, " to "); p += 3; continue; }   /* → */
        if (!strncmp(p, "\xC2\xB7", 2)) { sputs(&t, ", "); p += 2; continue; }         /* · */
        if (!strncmp(p, "\xC3\x97", 2)) { sputs(&t, " times "); p += 2; continue; }    /* × */
        /* A commit hash in backticks (`fe94352`): "a commit", not "four million
         * six hundred seventy one thousand ...". */
        if (c == '`') {
            const char *q = p + 1;
            int n = 0, digit = 0;
            while (q[n] && q[n] != '`' && n < 41 && (isdigit((unsigned char)q[n]) || (q[n] >= 'a' && q[n] <= 'f'))) {
                if (isdigit((unsigned char)q[n])) digit = 1;
                n++;
            }
            if (q[n] == '`' && n >= 7 && n <= 40 && digit) {
                sputs(&t, " a commit ");
                p = q + n + 1;
                continue;
            }
        }
        if (c >= 0x80) { p++; continue; }
        switch (c) {
        case '*': case '`': case '#': case '<': case '>': case '~': case '"': p++; continue;
        case '_': case '/': case '\\': sputc(&t, ' '); p++; continue;
        case '|': sputs(&t, ", "); p++; continue;
        case '%': sputs(&t, " percent "); p++; continue;
        case '&': sputs(&t, " and "); p++; continue;
        case '+': sputs(&t, " plus "); p++; continue;
        case '=': sputs(&t, " equals "); p++; continue;
        case ':':
            /* A clock time (7:06): "seven oh six", not "seven, zero six". */
            if (p > s && isdigit((unsigned char)p[-1]) && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) && !isdigit((unsigned char)p[3])) {
                if (p[1] == '0') {
                    if (p[2] == '0') sputs(&t, " o'clock"); else { sputs(&t, " oh "); sputc(&t, p[2]); }
                    p += 3;
                } else {
                    sputc(&t, ' ');
                    p++;
                }
                continue;
            }
            sputs(&t, ", ");
            p++;
            continue;
        case '(': case ')': sputs(&t, ", "); p++; continue;
        case '-':
            sputc(&t, ' ');
            p++;
            continue;
        case '.':
            /* Between two word characters (a file name, a version): "dot". */
            if (t.n && isalnum((unsigned char)t.p[t.n - 1]) && isalpha((unsigned char)p[1])) {
                sputs(&t, " dot ");
                p++;
                continue;
            }
            break;
        }
        sputc(&t, (char)c);
        p++;
    }
    /* Words as spoken. */
    const char *q = t.p ? t.p : "";
    while (*q) {
        if (isalnum((unsigned char)*q)) {
            const char *a = q;
            while (isalnum((unsigned char)*q) || (*q == '.' && isdigit((unsigned char)q[1]) && q > a && isdigit((unsigned char)q[-1])) || *q == '\'') q++;
            say_word(o, a, (int)(q - a));
        } else {
            sputc(o, *q);
            q++;
        }
    }
    free(t.p);
}

/* The markdown of a response as plain sentences, one per line: the
 * response delimiters and code dropped (a code block said once), table
 * rows as clauses, headings and list items as sentences of their own. */
static void markdown_to_speech(const char *md, Str *out) {
    int in_code = 0;
    const char *p = md;
    while (*p) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        char *line = malloc((size_t)len + 1);
        memcpy(line, p, (size_t)len);
        line[len] = 0;
        p += len + (e ? 1 : 0);
        char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, "```", 3) || !strncmp(l, "~~~", 3)) {
            if (!in_code) sputs(out, "code block.\n");
            in_code = !in_code;
            free(line);
            continue;
        }
        if (in_code || !*l) { free(line); continue; }
        /* The response delimiters, or a table's separator row: nothing. */
        if (strstr(l, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2")) { free(line); continue; }
        if (*l == '|' && strspn(l, "|-: ") == strlen(l)) { free(line); continue; }
        /* List markers. */
        if ((l[0] == '-' || l[0] == '*' || l[0] == '+') && l[1] == ' ') l += 2;
        else if (isdigit((unsigned char)l[0])) {
            char *d = l;
            while (isdigit((unsigned char)*d)) d++;
            if ((*d == '.' || *d == ')') && d[1] == ' ') l = d + 2;
        }
        Str s = {0};
        say_line(&s, l);
        /* Ends as a sentence, and begins with a word (a table row began
         * with its pipe's comma). */
        while (s.n && (isspace((unsigned char)s.p[s.n - 1]) || s.p[s.n - 1] == ',' || s.p[s.n - 1] == ':'))
            s.p[--s.n] = 0;
        size_t lead = 0;
        while (lead < s.n && (isspace((unsigned char)s.p[lead]) || s.p[lead] == ',')) lead++;
        if (s.n > lead) {
            sputs(out, s.p + lead);
            if (!strchr(".!?;", s.p[s.n - 1])) sputc(out, '.');
            sputc(out, '\n');
        }
        free(s.p);
        free(line);
    }
}

/* Sentences of plain text (split at . ! ? ; before a space, and at line
 * ends), each said on its own as SBTalker said each Say, with a pause. */
static void say_sentences(const char *text, int pitch, Bytes *pcm) {
    const char *p = text;
    Str sent = {0};
    while (1) {
        char c = *p;
        int end = c == 0 || c == '\n' ||
            (strchr(".!?;", c) && c && (p[1] == ' ' || p[1] == '\n' || p[1] == 0));
        if (c && c != '\n') sputc(&sent, c);
        if (end) {
            int letters = 0;
            for (size_t k = 0; k < sent.n; k++) if (isalpha((unsigned char)sent.p[k])) letters = 1;
            if (letters) {
                synthesise(sent.p, pitch, pcm, NULL);
                for (int k = 0; k < RATE / 6; k++) bput(pcm, 128);
            }
            sent.n = 0;
            if (sent.p) sent.p[0] = 0;
            if (!c) break;
            if (c != '\n' && p[1] == ' ') p++;
        }
        p++;
    }
    free(sent.p);
}

/* ── WAV out: unsigned 8-bit mono ───────────────────────────────────── */

static void le32(FILE *f, uint32_t v) { for (int k = 0; k < 4; k++) fputc((v >> (8 * k)) & 0xFF, f); }
static void le16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }

static void write_wav(FILE *f, const Bytes *b) {
    fwrite("RIFF", 1, 4, f); le32(f, 36 + (uint32_t)b->n);
    fwrite("WAVEfmt ", 1, 8, f); le32(f, 16); le16(f, 1); le16(f, 1);
    le32(f, RATE); le32(f, RATE); le16(f, 1); le16(f, 8);
    fwrite("data", 1, 4, f); le32(f, (uint32_t)b->n);
    if (b->n) fwrite(b->p, 1, b->n, f);
}

int main(int argc, char **argv) {
    int pitch = 5, g2p_only = 0, md = 0, text_only = 0;
    const char *outpath = NULL;
    Str text = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) pitch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(argv[i], "--godot")) godot_mode = 1;
        else if (!strcmp(argv[i], "--g2p")) g2p_only = 1;
        else if (!strcmp(argv[i], "--md")) md = 1;
        else if (!strcmp(argv[i], "--text")) text_only = md = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "tts079 [-p PITCH 0-9] [--godot] [--g2p] [--md] [--text] [-o FILE.wav] [TEXT...]\n"
                            "  --md    the input is a response's markdown: said sentence by sentence\n"
                            "  --text  print the sentences --md would say, and nothing else\n");
            return 0;
        } else { if (text.n) sputc(&text, ' '); sputs(&text, argv[i]); }
    }
    if (pitch < 0 || pitch > 9) { fprintf(stderr, "tts079: pitch is 0 to 9\n"); return 1; }
    if (!text.n) {
        int c;
        /* Markdown keeps its lines; plain text is one sentence stream. */
        while ((c = getchar()) != EOF) sputc(&text, (c == '\n' && !md) ? ' ' : (char)c);
    }
    load();
    if (g2p_only) {
        Str g = {0};
        g2p_sentence(text.p ? text.p : "", &g);
        printf("%s\n", g.p ? g.p : "");
        return 0;
    }
    Bytes pcm = {0};
    if (md) {
        Str plain = {0};
        markdown_to_speech(text.p ? text.p : "", &plain);
        if (text_only) {
            /* One sentence a line, split as say_sentences splits them. */
            const char *q = plain.p ? plain.p : "";
            Str sent = {0};
            for (;; q++) {
                char c = *q;
                int end = c == 0 || c == '\n' ||
                    (c && strchr(".!?;", c) && (q[1] == ' ' || q[1] == '\n' || q[1] == 0));
                if (c && c != '\n') sputc(&sent, c);
                if (end) {
                    size_t a = 0;
                    while (a < sent.n && sent.p[a] == ' ') a++;
                    int letters = 0;
                    for (size_t k = a; k < sent.n; k++) if (isalpha((unsigned char)sent.p[k])) letters = 1;
                    if (letters) printf("%s\n", sent.p + a);
                    sent.n = 0;
                    if (sent.p) sent.p[0] = 0;
                    if (!c) break;
                }
            }
            fflush(stdout);
            return 0;
        }
        say_sentences(plain.p ? plain.p : "", pitch, &pcm);
    } else {
        synthesise(text.p ? text.p : "", pitch, &pcm, NULL);
    }
    FILE *f = outpath ? fopen(outpath, "wb") : stdout;
    if (!f) { perror(outpath); return 1; }
    write_wav(f, &pcm);
    if (outpath) fclose(f);
    return 0;
}
