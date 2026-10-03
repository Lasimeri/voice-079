#!/bin/bash
# One-shot: when diag.md goes 45 s stale, count for 8 s whether the engine
# calls write_status_file / diag_text / finish_agent_wait and what it writes
# (gdb breakpoints, stall.gdb), then exit. Kept on disk, not tmpfs.
diag=~/.local/share/phi-stream/dev/diag.md; end=$(( $(date +%s) + 3600 ))
while [ "$(date +%s)" -lt "$end" ]; do
    p=$(pgrep -f 'target/release/phi-stre[a]m serve' | head -1); [ -n "$p" ] || { sleep 2; continue; }
    # still loading: diag.md is the previous instance's until the new one writes it
    [ "$(stat -c %Y "$diag")" -gt "$(( $(date +%s) - $(ps -o etimes= -p "$p") ))" ] || { sleep 2; continue; }
    age=$(( $(date +%s) - $(stat -c %Y "$diag") ))
    if [ "$age" -ge 45 ]; then
        out=~/.cache/voice-079/wedges/probe-$(date +%F_%H%M%S); mkdir -p "$out"
        p=$(pgrep -f 'target/release/phi-stre[a]m serve' | head -1)
        echo "diag age $age s, pid $p, $(date +%T)" > "$out/info"
        cp "$diag" "$out/diag.md"; cp ~/.local/share/phi-stream/dev/status.txt "$out/status.txt"
        timeout -s INT 8 gdb -batch -nx -p "$p" -x ~/.cache/voice-079/stall.gdb -ex continue \
            -ex 'printf "COUNTS write_status_file=%d diag_text=%d finish_agent_wait=%d fs_write=%d\n", $nw, $nd, $nfa, $nf' \
            -ex 'thread apply all bt 8' > "$out/gdb.txt" 2>&1
        echo "$out"; grep COUNTS "$out/gdb.txt"; exit 0
    fi
    sleep 2
done
echo "no stall within the hour"
