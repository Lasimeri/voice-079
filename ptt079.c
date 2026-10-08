/* ptt079: hold ` to talk to Claude. Takes over one keyboard (EVIOCGRAB) and
 * passes every event through a virtual keyboard (uinput), LEDs back the
 * other way, except the ` key alone:
 *   held past TAP_MS: talk; listen079 records from the press (SIGUSR1) and
 *     transcribes at the release (SIGUSR2);
 *   tapped quicker: listen079 drops the recording (SIGRTMIN) and a ` is typed;
 *   with a modifier down (Shift for ~, Ctrl, Alt, Meta): passed through.
 * If this process ends, the kernel releases the keyboard at once.
 *
 * Build: tcc -O2 -o ptt079 ptt079.c
 * Run:   ptt079 /dev/input/by-id/...-event-kbd   (listen079 --ptt running)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define TAP_MS 250
#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
#define TEST_BIT(b, a) (((a)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

static volatile sig_atomic_t stop;
static void on_stop(int s) { (void)s; stop = 1; }

static char state_dir[512];

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* listen079's pid, as it wrote it. */
static pid_t listener(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/listen079.pid", state_dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    long v = 0;
    if (fscanf(f, "%ld", &v) != 1) v = 0;
    fclose(f);
    return (pid_t)v;
}

static void tell(int sig) {
    pid_t l = listener();
    if (l > 0) kill(l, sig);
}

/* The status line's word for push-to-talk. */
static void ptt_state(const char *s) {
    char p[600];
    snprintf(p, sizeof p, "%s/ptt", state_dir);
    FILE *f = fopen(p, "w");
    if (f) { fputs(s, f); fclose(f); }
}

/* The mute: $dir/muted there or not (listen079 drops what it hears while
 * it is), the status line told, and a word in 079's voice. */
static void toggle_mute(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/muted", state_dir);
    int was = access(p, F_OK) == 0;
    if (was) unlink(p);
    else { FILE *f = fopen(p, "w"); if (f) fclose(f); }
    ptt_state(was ? "" : "muted");
    pid_t pid = fork();
    if (pid == 0) {
        char *home = getenv("HOME");
        char path[512];
        snprintf(path, sizeof path, "%s/tts079/speak079d", home ? home : "");
        execl(path, path, "say", was ? "Listening." : "Muted.", (char *)NULL);
        _exit(127);
    }
}

static void emit(int u, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (write(u, &e, sizeof e) != sizeof e) { /* the virtual device is gone */ stop = 1; }
}

/* A character on the US layout: its key, and whether Shift is down. */
static int us_key(char c, int *shift) {
    static const char *row_lo = "1234567890-=";
    static const char *row_hi = "!@#$%^&*()_+";
    static const int row_keys[] = {KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
        KEY_0, KEY_MINUS, KEY_EQUAL};
    static const int letters[] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
        KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V,
        KEY_W, KEY_X, KEY_Y, KEY_Z};
    static const struct { char lo, hi; int key; } sym[] = {
        {'[', '{', KEY_LEFTBRACE}, {']', '}', KEY_RIGHTBRACE}, {';', ':', KEY_SEMICOLON},
        {'\'', '"', KEY_APOSTROPHE}, {'`', '~', KEY_GRAVE}, {'\\', '|', KEY_BACKSLASH},
        {',', '<', KEY_COMMA}, {'.', '>', KEY_DOT}, {'/', '?', KEY_SLASH}};
    *shift = 0;
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c >= 'A' && c <= 'Z') { *shift = 1; return letters[c - 'A']; }
    if (c == ' ') return KEY_SPACE;
    for (int k = 0; k < 12; k++) {
        if (c == row_lo[k]) return row_keys[k];
        if (c == row_hi[k]) { *shift = 1; return row_keys[k]; }
    }
    for (size_t k = 0; k < sizeof sym / sizeof sym[0]; k++) {
        if (c == sym[k].lo) return sym[k].key;
        if (c == sym[k].hi) { *shift = 1; return sym[k].key; }
    }
    return -1;
}

static void tap(int u, int key) {
    emit(u, EV_KEY, key, 1);
    emit(u, EV_SYN, SYN_REPORT, 0);
    emit(u, EV_KEY, key, 0);
    emit(u, EV_SYN, SYN_REPORT, 0);
}

/* Caps Lock as the keyboard's LED shows it: letters typed with it on come
 * out inverted ("[VOICE] nO, IT IS..."), so Shift is flipped for them. */
static int caps_on;

/* What is heard goes to Claude Code whatever has the focus: claude-focus
 * (next to this program) gives its window the focus for the line and the
 * window that had it gets it back. */
static void focus(const char *how) {
    char exe[512], cmd[700];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char *s = strrchr(exe, '/');
    if (!s) return;
    *s = 0;
    snprintf(cmd, sizeof cmd, "\"%s/claude-focus\" %s", exe, how);
    if (system(cmd) != 0) return;
}

/* A line typed into the focused window, then Enter: what was heard. */
static void type_line(int u, const char *s) {
    for (; *s; s++) {
        int shift, key = us_key(*s, &shift);
        if (key < 0) continue;
        if (caps_on && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) shift = !shift;
        if (shift) { emit(u, EV_KEY, KEY_LEFTSHIFT, 1); emit(u, EV_SYN, SYN_REPORT, 0); }
        tap(u, key);
        if (shift) { emit(u, EV_KEY, KEY_LEFTSHIFT, 0); emit(u, EV_SYN, SYN_REPORT, 0); }
        usleep(2000);
    }
    usleep(20000);
    tap(u, KEY_ENTER);
}

static int is_mod(int code) {
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA:
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    /* --type-only: no push-to-talk; the keyboard is read for its keys and
     * Caps Lock, never taken, and lines heard are typed (open-mic mode). */
    int type_only = argc > 2 && !strcmp(argv[1], "--type-only");
    /* --mute: the keyboard taken and passed through whole, but for the
     * number pad's period, which mutes and unmutes the mic for Claude
     * (open-mic mode; the person: "a mute button ... the period key on the
     * number pad"). No push-to-talk. */
    int mute_key = argc > 2 && !strcmp(argv[1], "--mute");
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(state_dir, sizeof state_dir, "%s/speak-079", rt ? rt : "/tmp");
    /* --toggle: the same mute as the number pad's period, once, from
     * anything else (a tap on the Glass's touchpad, glass-tap.sh); no keyboard. */
    if (argc == 2 && !strcmp(argv[1], "--toggle")) {
        toggle_mute();
        return 0;
    }
    if (type_only || mute_key) { argv++; argc--; }
    if (argc < 2) { fprintf(stderr, "ptt079 [--type-only|--mute] /dev/input/...-event-kbd | ptt079 --toggle\n"); return 1; }

    int fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "ptt079: %s: %s\n", argv[1], strerror(errno)); return 1; }
    int u = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (u < 0) { fprintf(stderr, "ptt079: /dev/uinput: %s\n", strerror(errno)); return 1; }

    /* The virtual keyboard: the real one's keys, scan codes, LEDs, repeat. */
    unsigned long keys[NBITS(KEY_MAX + 1)], leds[NBITS(LED_MAX + 1)], msc[NBITS(MSC_MAX + 1)];
    memset(keys, 0, sizeof keys); memset(leds, 0, sizeof leds); memset(msc, 0, sizeof msc);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_LED, sizeof leds), leds);
    ioctl(fd, EVIOCGBIT(EV_MSC, sizeof msc), msc);
    ioctl(u, UI_SET_EVBIT, EV_KEY);
    ioctl(u, UI_SET_EVBIT, EV_SYN);
    ioctl(u, UI_SET_EVBIT, EV_MSC);
    ioctl(u, UI_SET_EVBIT, EV_LED);
    ioctl(u, UI_SET_EVBIT, EV_REP);
    for (int k = 0; k <= KEY_MAX; k++) if (TEST_BIT(k, keys)) ioctl(u, UI_SET_KEYBIT, k);
    for (int k = 0; k <= LED_MAX; k++) if (TEST_BIT(k, leds)) ioctl(u, UI_SET_LEDBIT, k);
    for (int k = 0; k <= MSC_MAX; k++) if (TEST_BIT(k, msc)) ioctl(u, UI_SET_MSCBIT, k);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x1b1c;
    us.id.product = 0x7079;
    snprintf(us.name, sizeof us.name, "ptt079 keyboard");
    if (ioctl(u, UI_DEV_SETUP, &us) < 0 || ioctl(u, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "ptt079: cannot create the virtual keyboard: %s\n", strerror(errno));
        return 1;
    }
    usleep(300000);   /* the compositor picks the new keyboard up */

    /* Keys held down now would never see their release through us. */
    unsigned long down[NBITS(KEY_MAX + 1)];
    for (int tries = 0; tries < 50; tries++) {
        memset(down, 0, sizeof down);
        ioctl(fd, EVIOCGKEY(sizeof down), down);
        int any = 0;
        for (size_t k = 0; k < sizeof down / sizeof down[0]; k++) if (down[k]) any = 1;
        if (!any) break;
        usleep(100000);
    }
    if (!type_only && ioctl(fd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "ptt079: cannot take the keyboard: %s\n", strerror(errno));
        ioctl(u, UI_DEV_DESTROY);
        return 1;
    }
    /* Caps Lock now. */
    unsigned long ledstate[NBITS(LED_MAX + 1)];
    memset(ledstate, 0, sizeof ledstate);
    if (ioctl(fd, EVIOCGLED(sizeof ledstate), ledstate) >= 0) caps_on = TEST_BIT(LED_CAPSL, ledstate);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    signal(SIGHUP, on_stop);
    ptt_state("");
    /* What listen079 heard, a line at a time, to type (voice079 writes it). */
    char fifo[600], line[4096];
    size_t line_n = 0;
    snprintf(fifo, sizeof fifo, "%s/type.fifo", state_dir);
    unlink(fifo);
    mkfifo(fifo, 0600);
    int tf = open(fifo, O_RDWR | O_NONBLOCK);
    fprintf(stderr, "ptt079: hold ` to talk\n");

    int mods = 0, talking = 0, passthrough = 0;
    long t_down = 0;
    struct pollfd pf[3] = {{type_only ? -1 : fd, POLLIN, 0}, {u, POLLIN, 0}, {tf, POLLIN, 0}};
    while (!stop) {
        if (poll(pf, tf >= 0 ? 3 : 2, 500) < 0) { if (errno == EINTR) continue; break; }
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;   /* unplugged */
        struct input_event e;
        while (pf[0].revents & POLLIN && read(fd, &e, sizeof e) == sizeof e) {
            if (e.type == EV_KEY && is_mod(e.code)) {
                if (e.value == 1) mods++;
                else if (e.value == 0 && mods > 0) mods--;
            }
            if (mute_key && e.type == EV_KEY && e.code == KEY_KPDOT) {
                if (e.value == 1) toggle_mute();
                continue;
            }
            if (!mute_key && e.type == EV_KEY && e.code == KEY_GRAVE && !passthrough) {
                if (e.value == 1) {
                    if (mods) {
                        passthrough = 1;
                    } else {
                        t_down = now_ms();
                        talking = 1;
                        tell(SIGUSR1);
                        ptt_state("listening");
                        continue;
                    }
                } else if (talking) {
                    if (e.value == 2) continue;   /* its autorepeat */
                    talking = 0;
                    ptt_state("");
                    if (now_ms() - t_down < TAP_MS) {
                        tell(SIGRTMIN);
                        emit(u, EV_KEY, KEY_GRAVE, 1);
                        emit(u, EV_SYN, SYN_REPORT, 0);
                        emit(u, EV_KEY, KEY_GRAVE, 0);
                        emit(u, EV_SYN, SYN_REPORT, 0);
                    } else {
                        tell(SIGUSR2);
                    }
                    continue;
                }
            }
            if (e.type == EV_KEY && e.code == KEY_GRAVE && passthrough && e.value == 0) passthrough = 0;
            /* LEDs are output: the K70 reports the LED state written to it, and
             * forwarded back in, that echo toggled Caps Lock over and over. */
            if (e.type == EV_LED) continue;
            emit(u, e.type, e.code, e.value);
        }
        /* Lines heard: typed into the focused window, each with Enter. */
        if (tf >= 0 && (pf[2].revents & POLLIN)) {
            char chunk[1024];
            ssize_t got;
            while ((got = read(tf, chunk, sizeof chunk)) > 0) {
                for (ssize_t k = 0; k < got; k++) {
                    if (chunk[k] == '\n') {
                        line[line_n] = 0;
                        if (line_n) {
                            focus("in");
                            usleep(60000);   /* the keyboard focus moves */
                            type_line(u, line);
                            usleep(40000);   /* Enter lands before it moves back */
                            focus("out");
                        }
                        line_n = 0;
                    } else if (line_n + 1 < sizeof line) line[line_n++] = chunk[k];
                }
            }
        }
        /* The lock keys' LEDs, set on the virtual keyboard, go to the real one. */
        while (pf[1].revents & POLLIN && read(u, &e, sizeof e) == sizeof e) {
            if (e.type == EV_LED) {
                if (e.code == LED_CAPSL) caps_on = e.value != 0;
                struct input_event l[2];
                memset(l, 0, sizeof l);
                l[0].type = EV_LED; l[0].code = e.code; l[0].value = e.value;
                l[1].type = EV_SYN; l[1].code = SYN_REPORT;
                if (write(fd, l, sizeof l) < 0) { /* LEDs are cosmetic */ }
            }
        }
    }
    if (talking) tell(SIGRTMIN);
    ptt_state("");
    ioctl(fd, EVIOCGRAB, 0);
    ioctl(u, UI_DEV_DESTROY);
    close(u);
    close(fd);
    return 0;
}
