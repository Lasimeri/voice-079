#!/bin/bash
# phi-stream-watchdog.sh: keeps the Phi Stream dev service alive. The service
# has twice wedged (host threads spinning at 300%+ CPU, waiting on card work
# that never came back, diag.md frozen) while the process stayed up, so a
# liveness check on the process alone never noticed. This watches diag.md,
# which the engine rewrites every 5 s: stale for STALE seconds with the
# process up means wedged, and the service is restarted with its exact
# command. Grace after each (re)start for loading the 35B onto GPU + cards;
# at most MAXR restarts an hour; leaves the service alone while
# scripts/improve-measure.sh is running (it restarts the service itself).
#   start: setsid phi-stream-watchdog.sh &     log: ~/.cache/voice-079/phi-watchdog.log
#   stop:  kill $(cat $XDG_RUNTIME_DIR/speak-079/phi-watchdog.pid)
#   once:  phi-stream-watchdog.sh restart     (restart the service now, then exit)
set -u
STALE=${PHI_WD_STALE:-180} GRACE=${PHI_WD_GRACE:-420} MAXR=${PHI_WD_MAXR:-6}
repo="$HOME/Intel Phi Stream"
ws="$HOME/.local/share/phi-stream/dev"
diag="$ws/diag.md"
log="$HOME/.cache/voice-079/phi-watchdog.log"
rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/speak-079"
mkdir -p "$rt" "$(dirname "$log")"
say() { printf '%s %s\n' "$(date '+%F %T')" "$*" >> "$log"; }

serve_pid() { pgrep -f 'target/release/phi-stream serve' | head -1; }

relaunch() {
    local p; p=$(serve_pid)
    if [ -n "$p" ]; then
        kill "$p" 2>/dev/null
        for _ in $(seq 1 20); do kill -0 "$p" 2>/dev/null || break; sleep 0.5; done
        kill -9 "$p" 2>/dev/null
    fi
    # The repository's own launcher, as a person and the measurement use it
    # (one command line for every start, phi-stream.opts): `restart` quits
    # the service however it was started, waits for it (two minutes, then
    # kills it) and starts the next in tmux, its window kept. libptracer.so
    # (PHI_STREAM_PRELOAD) lets evidence() take a wedge's thread stacks.
    local pre="" opts
    [ -f "$HOME/tts079/libptracer.so" ] && pre="$HOME/tts079/libptracer.so"
    opts=$(cat "$HOME/tts079/phi-stream.opts" 2>/dev/null)
    # shellcheck disable=SC2086
    (cd "$repo" && PHI_STREAM_PRELOAD="$pre" setsid scripts/phi-stream.sh restart dev $opts > /dev/null 2>&1 < /dev/null &)
    say "relaunched the service (was pid ${p:-none})"
}

# What the wedged service was doing, kept before it is restarted, so the
# cause can be found: every thread's stack (eu-stack; needs libptracer.so),
# the threads' states and CPU, both cards' workers (alive? control words),
# the tail of the service log and the frozen diag.md.
evidence() {   # PID
    local d="$HOME/.cache/voice-079/wedges/$(date +%F_%H%M%S)" p=$1 t
    mkdir -p "$d"
    # gdb, not eu-stack: eu-stack took over 90 s and gave nothing on this
    # 20+ GB process. Only ever at a wedge: attaching pauses every thread.
    timeout 240 gdb -batch -nx -p "$p" -ex "set pagination off" -ex "set print thread-events off" \
        -ex "thread apply all bt 14" > "$d/stacks.txt" 2>&1
    for t in /proc/"$p"/task/*; do
        printf '%s %s %s %s\n' "${t##*/}" "$(awk '{print $3, $14+$15}' "$t/stat" 2>/dev/null)" "$(cat "$t/wchan" 2>/dev/null)" "$(cat "$t/comm" 2>/dev/null)"
    done > "$d/threads.txt"
    for c in 0 1; do
        { echo "== card $c status"; timeout 20 "$HOME/Intel Phi AVX-512/scripts/phi-vpu.sh" -c "$c" status 2>&1
          echo "== card $c log (tail)"; timeout 20 "$HOME/Intel Phi AVX-512/scripts/phi-vpu.sh" -c "$c" log 2>&1 | tail -40; } > "$d/card$c.txt"
    done
    tail -60 "$HOME/.local/share/phi-stream/serve.log" > "$d/serve-tail.txt" 2>&1
    cp "$diag" "$d/diag.md" 2>/dev/null
    say "evidence kept in $d ($(grep -c '^Thread ' "$d/stacks.txt" 2>/dev/null) thread stacks)"
}

if [ "${1:-}" = restart ]; then relaunch; exit 0; fi

echo $$ > "$rt/phi-watchdog.pid"
say "watchdog up (stale ${STALE}s, grace ${GRACE}s, max ${MAXR}/h)"
last=$(date +%s); restarts=()
while :; do
    sleep 30
    now=$(date +%s)
    [ $(( now - last )) -lt "$GRACE" ] && continue
    pgrep -f 'improve-measure[.]sh' > /dev/null && { last=$now; continue; }
    p=$(serve_pid)
    [ -n "$p" ] || continue                      # not running: not ours to start
    # A service started by anyone (phi-stream.sh, a measurement, a login)
    # loads for minutes with the last one's diag.md: young is loading.
    [ "$(ps -o etimes= -p "$p" | tr -d ' ')" -lt "$GRACE" ] 2>/dev/null && continue
    [ -f "$diag" ] || continue
    age=$(( now - $(stat -c %Y "$diag") ))
    [ "$age" -lt "$STALE" ] && continue
    # keep only restarts from the last hour
    r=(); for t in "${restarts[@]:-}"; do [ -n "$t" ] && [ $(( now - t )) -lt 3600 ] && r+=("$t"); done; restarts=("${r[@]:-}")
    if [ "${#r[@]}" -ge "$MAXR" ]; then say "wedged (diag ${age}s old) but ${MAXR} restarts this hour; leaving it for a person"; last=$now; continue; fi
    say "wedged: diag.md ${age}s old, pid $p at $(ps -o pcpu= -p "$p" | tr -d ' ')% CPU"
    evidence "$p"
    relaunch
    restarts+=("$now"); last=$(date +%s)
done
