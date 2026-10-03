#!/bin/sh
# The guide lane and the expert capture, interleaved, the second chain on
# throughout: (guide off, experts off), (guide on, experts off), (guide on,
# experts on), twice, 5 min each. Per window: mean stream tok/s, checks,
# reflections told, loopiness, and the guide's reports in the window.
B="$HOME/Intel Phi Stream/target/release/phi-stream"
D="$HOME/.local/share/phi-stream/dev"
M="$HOME/.cache/phi-stream-tools/loopiness"
OUT="$HOME/.local/share/phi-stream/gate/guide-$(date +%H%M).txt"
num() { echo "$1" | grep -o "$2 [0-9]*" | head -1 | grep -o "[0-9]*$"; }
"$B" chain on >/dev/null 2>&1
for arm in "off off" "on off" "on on" "off off" "on off" "on on"; do
  set -- $arm
  "$B" guide $1 >/dev/null 2>&1; "$B" experts $2 >/dev/null 2>&1
  s0=$("$B" status); a=$(date +%s%6N)
  r0=$(grep -ac "your second look" "$D/stream.log")
  g0=$(wc -l < "$D/guide.log" 2>/dev/null || echo 0)
  sum=0; n=0
  for i in $(seq 30); do
    sleep 10
    r=$("$B" status | grep -o "stream [0-9.]* tok/s" | grep -o "[0-9.]*")
    [ -n "$r" ] && { sum=$(echo "$sum + $r" | bc); n=$((n+1)); }
  done
  s1=$("$B" status); b=$(date +%s%6N)
  r1=$(grep -ac "your second look" "$D/stream.log")
  g1=$(wc -l < "$D/guide.log" 2>/dev/null || echo 0)
  c=$(( $(num "$s1" checks) - $(num "$s0" checks) ))
  ch=$(( $(num "$s1" changed) - $(num "$s0" changed) ))
  tps=$(echo "scale=1; $sum / $n" | bc)
  loop=$("$M" "$D/chain.log" $a $b)
  # The guide's own lines in the window: tokens measured, mean KL, flips, experts shared.
  gl=$(tail -n $((g1-g0)) "$D/guide.log" 2>/dev/null | awk -F'\t' '
    { n++; for (i=1;i<=NF;i++) { split($i,kv,"="); if (kv[1]=="kl") kl+=kv[2]; if (kv[1]=="flip") f+=kv[2]; if (kv[1]=="experts_shared") { s+=kv[2]; sn++ } } }
    END { if (n) printf "guide %d tokens, KL %.3f, flips %.1f%%", n, kl/n, 100*f/n; else printf "guide 0 tokens"; if (sn) printf ", experts shared %.1f%%", 100*s/sn }')
  echo "$(date +%H:%M:%S) guide $1 experts $2: stream $tps tok/s; checks $c, changed $ch; reflections told $((r1-r0)); $gl; $loop" >> "$OUT"
done
"$B" guide off >/dev/null 2>&1; "$B" experts off >/dev/null 2>&1
echo "done $OUT" >> "$OUT"
