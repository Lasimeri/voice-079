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
    setsid zsh -c "cd \"$repo\" && env PHI_STREAM_BIN=\"$repo/target/release/phi-stream\" \"$repo/scripts/phi-stream.sh\" serve --dev \"$repo\" --mind --reflect --terminal --rollover-tokens 150000 --second-chain --workspace \"$ws\" -c 204800 --kv-q8 --frame agent --temp 0.5 --top-p 0.95 --min-p 0.05 --guide --goal-probe --chain-against --improve 2>&1 | tee -a \"$HOME/.local/share/phi-stream/serve.log\"" > /dev/null 2>&1 < /dev/null &
    say "relaunched the service (was pid ${p:-none})"
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
    [ -f "$diag" ] || continue
    age=$(( now - $(stat -c %Y "$diag") ))
    [ "$age" -lt "$STALE" ] && continue
    # keep only restarts from the last hour
    r=(); for t in "${restarts[@]:-}"; do [ -n "$t" ] && [ $(( now - t )) -lt 3600 ] && r+=("$t"); done; restarts=("${r[@]:-}")
    if [ "${#r[@]}" -ge "$MAXR" ]; then say "wedged (diag ${age}s old) but ${MAXR} restarts this hour; leaving it for a person"; last=$now; continue; fi
    say "wedged: diag.md ${age}s old, pid $p at $(ps -o pcpu= -p "$p" | tr -d ' ')% CPU"
    relaunch
    restarts+=("$now"); last=$(date +%s)
done
