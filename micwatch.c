/* micwatch: whether a microphone hears someone speaking, once a second, as a
 * status line for follow079 (the audio follows the person). Reads 16 kHz
 * mono s16le PCM on stdin (parec --raw), in 20 ms frames: a frame is speech
 * when its level stands SPEECH_DB above the noise floor (the floor follows
 * the quiet frames down at once and up slowly, so a steady noise becomes
 * floor). A second is voiced when at least 30 % of its frames are speech.
 * Each second PREFIX.status is rewritten (temp name, then renamed) and the
 * line appended to PREFIX.log:
 *   t=UNIX_US mic=NAME speech=0|1 level=DB floor=DB
 * Energy only: it tells a voice from quiet, not whose voice it is.
 *   parec ... | micwatch NAME PREFIX
 * Build: tcc -o micwatch micwatch.c -lm   (or gcc -O2 ... -lm) */
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#define RATE 16000
#define FRAME 320          /* 20 ms */
#define SPEECH_DB 12.0
#define VOICED_SHARE 0.30

static long long now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "micwatch NAME PREFIX\n"); return 2; }
    const char *name = argv[1], *prefix = argv[2];
    char st[4096], tmp[4096], lg[4096], lg1[4096];
    snprintf(st, sizeof st, "%s.status", prefix);
    snprintf(tmp, sizeof tmp, "%s.status.tmp", prefix);
    snprintf(lg, sizeof lg, "%s.log", prefix);
    snprintf(lg1, sizeof lg1, "%s.log.1", prefix);
    short buf[FRAME];
    double floor_db = -60.0, sum_db = 0;
    int frames = 0, speech = 0;
    while (fread(buf, sizeof(short), FRAME, stdin) == FRAME) {
        double e = 0;
        for (int i = 0; i < FRAME; i++) e += (double)buf[i] * buf[i];
        double db = 10.0 * log10(e / FRAME / (32768.0 * 32768.0) + 1e-12);
        if (db < floor_db) floor_db = db;               /* down at once */
        else floor_db += 0.002 * (db - floor_db);      /* up slowly, about 10 s */
        if (db > floor_db + SPEECH_DB) speech++;
        sum_db += db;
        if (++frames == RATE / FRAME) {
            int voiced = speech >= VOICED_SHARE * frames;
            char line[256];
            snprintf(line, sizeof line, "t=%lld mic=%s speech=%d level=%.1f floor=%.1f\n",
                     now_us(), name, voiced, sum_db / frames, floor_db);
            FILE *f = fopen(tmp, "w");
            if (f) { fputs(line, f); fclose(f); rename(tmp, st); }
            struct stat s;
            if (stat(lg, &s) == 0 && s.st_size > 2 * 1024 * 1024) rename(lg, lg1);
            f = fopen(lg, "a");
            if (f) { fputs(line, f); fclose(f); }
            frames = speech = 0;
            sum_db = 0;
        }
    }
    return 0;
}
