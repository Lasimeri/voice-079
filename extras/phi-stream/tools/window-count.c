/* window-count.c: live tokens placed in [a, b) microseconds, from a
   phi-stream stream.log ("t<TAB>kind<TAB>text" per piece; kinds think and
   speak are the live chain's tokens, given is from outside).
   Usage: window-count STREAM_LOG A_US B_US */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s LOG A B\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    long long a = atoll(argv[2]), b = atoll(argv[3]), n = 0;
    static char line[1 << 16];
    while (fgets(line, sizeof line, f)) {
        long long t = atoll(line);
        if (t < a || t >= b) continue;
        char *k = strchr(line, '\t');
        if (!k) continue;
        k++;
        if (!strncmp(k, "think\t", 6) || !strncmp(k, "speak\t", 6)) n++;
    }
    printf("%lld\n", n);
    return 0;
}
