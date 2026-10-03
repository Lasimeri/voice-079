#!/bin/sh
# guide-mix 0, 0.5, 1 interleaved twice (5 min each), the second chain,
# the guide and the experts on throughout. Per window: stream tok/s,
# checks (changed), tool calls that did not parse, messages to Claude,
# reflections told, the guide's own numbers, loopiness.
B="$HOME/Intel Phi Stream/target/release/phi-stream"
D="$HOME/.local/share/phi-stream/dev"
M="$HOME/.cache/phi-stream-tools/loopiness"
OUT="$HOME/.local/share/phi-stream/gate/mix-$(date +%H%M).txt"
num() { echo "$1" | grep -o "$2 [0-9]*" | head -1 | grep -o "[0-9]*$"; }
"$B" chain on >/dev/null 2>&1; "$B" guide on >/dev/null 2>&1; "$B" experts on >/dev/null 2>&1
for mix in 0 0.5 1 0 0.5 1; do
  "$B" set guide-mix $mix >/dev/null 2>&1
  s0=$("$B" status); a=$(date +%s%6N)
  r0=$(grep -ac "your second look" "$D/stream.log")
  p0=$(grep -ac "did not parse" "$D/stream.log")
  m0=$(grep -c "^## m" "$D/to-claude.md" 2>/dev/null || echo 0)
  g0=$(wc -l < "$D/guide.log")
  sum=0; n=0
  for i in $(seq 30); do
    sleep 10
    r=$("$B" status | grep -o "stream [0-9.]* tok/s" | grep -o "[0-9.]*")
    [ -n "$r" ] && { sum=$(echo "$sum + $r" | bc); n=$((n+1)); }
  done
  s1=$("$B" status); b=$(date +%s%6N)
  r1=$(grep -ac "your second look" "$D/stream.log")
  p1=$(grep -ac "did not parse" "$D/stream.log")
  m1=$(grep -c "^## m" "$D/to-claude.md" 2>/dev/null || echo 0)
  g1=$(wc -l < "$D/guide.log")
  c=$(( $(num "$s1" checks) - $(num "$s0" checks) ))
  ch=$(( $(num "$s1" changed) - $(num "$s0" changed) ))
  tps=$(echo "scale=1; $sum / $n" | bc)
  loop=$("$M" "$D/chain.log" $a $b)
  gl=$(tail -n $((g1-g0)) "$D/guide.log" | awk -F'\t' '
    { n++; for (i=1;i<=NF;i++) { split($i,kv,"="); if (kv[1]=="kl") kl+=kv[2]; if (kv[1]=="flip") f+=kv[2]; if (kv[1]=="experts_shared") { s+=kv[2]; sn++ } } }
    END { if (n) printf "guide %d tokens, KL %.3f, flips %.1f%%", n, kl/n, 100*f/n; else printf "guide 0 tokens"; if (sn) printf ", experts shared %.1f%%", 100*s/sn }')
  echo "$(date +%H:%M:%S) mix $mix: stream $tps tok/s; checks $c, changed $ch; unparsed calls $((p1-p0)); to Claude $((m1-m0)); reflections $((r1-r0)); $gl; $loop" >> "$OUT"
done
"$B" set guide-mix 0 >/dev/null 2>&1
echo "done $OUT" >> "$OUT"
