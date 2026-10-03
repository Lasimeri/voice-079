#!/bin/bash
# Throughput with the shadow guide lane on and off, interleaved (this host
# drifts): on, off, on, off, MIN minutes each, the stream's own tok/s from
# `phi-stream status` every 10 s (first minute of each window dropped).
B="$HOME/Intel Phi Stream/target/release/phi-stream"; min=${1:-4}; out=~/.cache/phi-stream-tools/guide-rate-ab.txt
: > "$out"
for arm in on off on off; do
    "$B" guide "$arm" > /dev/null 2>&1
    sleep 60
    n=$(( (min - 1) * 6 ))
    for _ in $(seq 1 $n); do
        s=$("$B" status 2>/dev/null | head -1)
        r=$(printf '%s' "$s" | grep -oP 'stream \K[0-9.]+(?= tok/s)'); b=$(printf '%s' "$s" | grep -oP 'beside \K[0-9.]+(?= tok/s)')
        st=$(printf '%s' "$s" | awk '{print $1}')
        echo "$arm $(date +%T) $st ${r:-NA} ${b:-NA}" >> "$out"
        sleep 10
    done
done
"$B" guide on > /dev/null 2>&1
awk '$4 != "NA" && $3 ~ /thinking|speaking/ {s[$1]+=$4; n[$1]++} END {for (a in s) printf "%s: %.2f tok/s over %d samples\n", a, s[a]/n[a], n[a]}' "$out"
