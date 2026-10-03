/* loopiness.c: how much of the live chain repeats itself, from a
   phi-stream chain.log ("t<TAB>kind<TAB>piece" per token): of the 8-token
   sequences written in [a, b) microseconds, the share that already
   occurred earlier in that window (0: none repeats; near 1: a loop).
   Usage: loopiness CHAIN_LOG A_US B_US */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define N 8
#define SLOTS (1 << 20)
static uint64_t set[SLOTS];
static uint64_t fnv(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s && *s != '\n') { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h ? h : 1;
}
static int seen_or_add(uint64_t h) {
    uint64_t i = h & (SLOTS - 1);
    while (set[i]) { if (set[i] == h) return 1; i = (i + 1) & (SLOTS - 1); }
    set[i] = h;
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s LOG A B\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    long long a = atoll(argv[2]), b = atoll(argv[3]);
    static char line[1 << 16];
    uint64_t win[N] = {0};
    long long n = 0, grams = 0, repeats = 0, injected = 0;
    while (fgets(line, sizeof line, f)) {
        long long t = atoll(line);
        if (t < a || t >= b) continue;
        char *k = strchr(line, '\t');
        if (!k) continue;
        k++;
        if (!strncmp(k, "given\t", 6) && strstr(k, "from the system")) injected++;
        if (strncmp(k, "think\t", 6) && strncmp(k, "speak\t", 6)) continue;
        win[n % N] = fnv(k + 6);
        n++;
        if (n < N) continue;
        uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < N; i++) { h ^= win[(n + i) % N]; h *= 1099511628211ULL; }
        grams++;
        repeats += seen_or_add(h ? h : 1);
    }
    printf("tokens %lld, 8-grams %lld, repeated %lld (%.3f), system lines %lld\n", n, grams, repeats,
           grams ? (double)repeats / grams : 0.0, injected);
    return 0;
}
