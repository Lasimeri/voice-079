#!/bin/bash
# Interleaved windows of the guide's aside source: placebo, lens, chain, three
# rounds of 5 minutes each, on the live service. Boundaries to the gate log.
set -u
bin="/home/lasimeri/Intel Phi Stream/target/release/phi-stream"
out="$HOME/.local/share/phi-stream/gate/src-windows-$(date +%Y%m%d-%H%M).log"
win=${WIN:-300}
for round in 1 2 3; do
    for src in placebo lens chain; do
        "$bin" guide "$src" > /dev/null
        t0=$(date +%s%6N)
        sleep "$win"
        t1=$(date +%s%6N)
        printf '%s\t%s\t%s\t%s\n' "$round" "$src" "$t0" "$t1" >> "$out"
    done
done
"$bin" guide chain > /dev/null
echo "$out"
