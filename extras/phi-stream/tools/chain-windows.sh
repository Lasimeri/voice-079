#!/bin/sh
# Interleaved windows: the second chain off, on, off, on (5 min each), on
# the running dev service. Per window: mean stream tok/s (status every
# 10 s), checks / unparsed / changed deltas, reflections told, messages to
# Claude, loopiness of its tokens (the loopiness meter).
B="$HOME/Intel Phi Stream/target/release/phi-stream"
D="$HOME/.local/share/phi-stream/dev"
M="$HOME/.cache/phi-stream-tools/loopiness"
OUT="$HOME/.local/share/phi-stream/gate/agent-chain-$(date +%H%M).txt"
num() { echo "$1" | grep -o "$2 [0-9]*" | head -1 | grep -o "[0-9]*$"; }
for mode in off on off on; do
  "$B" chain $mode >/dev/null 2>&1
  s0=$("$B" status); a=$(date +%s%6N)
  r0=$(grep -ac "your second look" "$D/stream.log"); m0=$(grep -c "^## m" "$D/to-claude.md" 2>/dev/null || echo 0)
  sum=0; n=0
  for i in $(seq 30); do
    sleep 10
    r=$("$B" status | grep -o "stream [0-9.]* tok/s" | grep -o "[0-9.]*")
    [ -n "$r" ] && { sum=$(echo "$sum + $r" | bc); n=$((n+1)); }
  done
  s1=$("$B" status); b=$(date +%s%6N)
  r1=$(grep -ac "your second look" "$D/stream.log"); m1=$(grep -c "^## m" "$D/to-claude.md" 2>/dev/null || echo 0)
  c=$(( $(num "$s1" checks) - $(num "$s0" checks) ))
  u=$(( $(num "$s1" unparsed) - $(num "$s0" unparsed) ))
  ch=$(( $(num "$s1" changed) - $(num "$s0" changed) ))
  tps=$(echo "scale=1; $sum / $n" | bc)
  loop=$("$M" "$D/chain.log" $a $b)
  echo "$(date +%H:%M:%S) chain $mode: stream $tps tok/s; checks $c, unparsed $u, changed $ch; reflections told $((r1-r0)); to Claude $((m1-m0)); $loop" >> "$OUT"
done
echo "done $OUT" >> "$OUT"
